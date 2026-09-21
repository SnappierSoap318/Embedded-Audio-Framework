#include "board_output.h"
#include <eaf/eaf_core.h>
#include <eaf/eaf_dsp.h>
#include <stdarg.h>
#include <stdio.h>
static eaf_board_log_fn log_sink;
static int audio_cpu = -1;
static eaf_reservoir_t reservoir;
static eaf_pipeline_t pipeline;
static int32_t *storage;
static uint32_t capacity;
static eaf_sink_t *sink;
static uint8_t input_channels;
static eaf_volume_ctx_t volume = {{INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX}};
static eaf_node_t master = {"master volume", EAF_NODE_STAGE_POST_PROCESS, &eaf_volume_ops, &volume};
/* Fixed -10 dB bench attenuation even if the protocol requests full volume.
   Q1.31 gain = round(2^31 * 10^(-10/20)) = 679093957. */
static eaf_volume_ctx_t attenuation = {{679093957, 679093957, 679093957, 679093957}};
static eaf_node_t limiter = {"bench attenuation", EAF_NODE_STAGE_PRE_PROCESS, &eaf_volume_ops,
                             &attenuation};
static eaf_node_t *const nodes[] = {&limiter, &master};
/* All three atomics are sequentially consistent: one bounded coherent read per block.
   A concurrent update is retried next block, never spun on in the audio task. */
static atomic_uint gain_sequence, gain_left = INT32_MAX, gain_right = INT32_MAX;
static eaf_thread_t audio;
static eaf_atomic_u32_t quit, elapsed, played, failed, pause_request, pause_ack, done, run_gate;
static bool active;
static eaf_atomic_u32_t underruns, queue_min, process_calls;

static void output_log(const char *format, ...) {
    if (!log_sink)
        return;
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log_sink(buffer);
}

static int set_volume(void *ctx, int32_t left, int32_t right) {
    (void)ctx;
    atomic_fetch_add(&gain_sequence, 1u);
    atomic_store(&gain_left, (unsigned)left);
    atomic_store(&gain_right, (unsigned)right);
    atomic_fetch_add(&gain_sequence, 1u);
    return EAF_OK;
}
static void apply_volume(void) {
    unsigned before = atomic_load(&gain_sequence);
    if (before & 1u)
        return;
    unsigned left = atomic_load(&gain_left), right = atomic_load(&gain_right);
    if (before != atomic_load(&gain_sequence))
        return;
    volume.gain[0] = (int32_t)left;
    volume.gain[1] = (int32_t)right;
}
static void consume(void *ctx) {
    (void)ctx;
    /* No graph access until START has reset the reservoir and enabled output. */
    while (!hal_atomic_get(&run_gate) && !hal_atomic_get(&quit))
        hal_sleep_ms(1);
    while (!hal_atomic_get(&quit)) {
        if (hal_atomic_get(&pause_request)) {
            if (board_sink_pause(true)) {
                hal_atomic_set(&failed, 1);
                break;
            }
            hal_atomic_set(&pause_ack, 1);
            while (hal_atomic_get(&pause_request) && !hal_atomic_get(&quit))
                hal_sleep_ms(1);
            if (board_sink_pause(false)) {
                hal_atomic_set(&failed, 1);
                break;
            }
            hal_atomic_set(&pause_ack, 0);
            continue;
        }
        apply_volume();
        int rc = eaf_pipeline_process(&pipeline);
        hal_atomic_set(&process_calls, hal_atomic_get(&process_calls) + 1u);
        if (rc && rc != EAF_EOF) {
            output_log("Audio process failed rc=%d state=%d", rc, (int)pipeline.state);
            hal_atomic_set(&failed, 1);
            break;
        }
        hal_atomic_set(&underruns, reservoir.underruns);
        uint64_t presented = reservoir.frames_read;
        if (presented && rc != EAF_EOF) {
            uint32_t level = eaf_reservoir_level(&reservoir);
            if (level < hal_atomic_get(&queue_min))
                hal_atomic_set(&queue_min, level);
        }
        /* Submitted frames, not a speaker presentation timestamp. */
        hal_atomic_set(&elapsed, (uint32_t)(presented * 1000u / reservoir.format.sample_rate));
        if (presented)
            hal_atomic_set(&played, 1);
        if (rc == EAF_EOF)
            break;
    }
    hal_atomic_set(&done, 1);
}
static int pause_output(bool paused) {
    output_log("Output pause request=%u", paused ? 1u : 0u);
    hal_atomic_set(&pause_request, paused ? 1u : 0u);
    /* Before initial release the worker cannot touch graph or sink yet. */
    if (!hal_atomic_get(&run_gate)) {
        hal_atomic_set(&pause_ack, paused ? 1u : 0u);
        return EAF_OK;
    }
    uint64_t deadline = hal_monotonic_time_us() + 1500000u;
    while (hal_atomic_get(&pause_ack) != (paused ? 1u : 0u) && !hal_atomic_get(&done)) {
        if (hal_monotonic_time_us() > deadline)
            return EAF_IO;
        hal_sleep_ms(1);
    }
    return hal_atomic_get(&failed) ? EAF_IO : EAF_OK;
}
void eaf_board_output_stop(void) {
    if (!active)
        return;
    hal_atomic_set(&quit, 1);
    if (audio.impl && hal_thread_join(&audio)) {
        hal_atomic_set(&failed, 1);
        return; /* Retain resources until quiescence can be established. */
    }
    if ((pipeline.state == EAF_RUNNING || pipeline.state == EAF_RECOVERY) &&
        eaf_pipeline_stop(&pipeline)) {
        hal_atomic_set(&failed, 1);
        return;
    }
    output_log("Stopped: source frames=%llu, underruns=%u",
               (unsigned long long)reservoir.frames_read, reservoir.underruns);
    if (eaf_pipeline_deinit(&pipeline)) {
        hal_atomic_set(&failed, 1);
        return;
    }
    active = false;
}
int eaf_board_output_start(const eaf_format_t *format, uint32_t ready_frames, bool held) {
    if (active) {
        /* A finished stream (EOF drained by the worker) is reaped so the next
           track can start; a genuinely running stream is a state error. */
        if (!hal_atomic_get(&done))
            return EAF_STATE;
        eaf_board_output_stop();
    }
    if (audio.impl || pipeline.state != EAF_UNINITIALIZED)
        return EAF_STATE;
    if (!format || !eaf_format_valid(format) || format->num_channels > 2)
        return EAF_UNSUPPORTED;
    if (!ready_frames || ready_frames > capacity)
        ready_frames = capacity * 3u / 4u;
    output_log("Output request: %u Hz channels=%u", format->sample_rate,
               (unsigned)format->num_channels);
    input_channels = format->num_channels;
    eaf_pipeline_config_t config = {&reservoir, nodes, 2, sink};
    int rc = eaf_reservoir_init(&reservoir, storage, capacity,
                                (eaf_format_t){format->sample_rate, 2, 3}, ready_frames);
    if (!rc)
        rc = eaf_pipeline_init(&pipeline, &config);
    if (!rc)
        rc = eaf_pipeline_configure(&pipeline, 128);
    if (rc) {
        output_log("Output setup failed rc=%d", rc);
        int cleanup = eaf_pipeline_deinit(&pipeline);
        if (cleanup) {
            active = true;
            hal_atomic_set(&failed, 1);
        }
        return cleanup ? cleanup : rc;
    }
    hal_atomic_set(&quit, 0);
    hal_atomic_set(&run_gate, 0);
    hal_atomic_set(&elapsed, 0);
    hal_atomic_set(&underruns, 0);
    hal_atomic_set(&queue_min, UINT32_MAX);
    hal_atomic_set(&played, 0);
    hal_atomic_set(&failed, 0);
    hal_atomic_set(&pause_request, 0);
    hal_atomic_set(&pause_ack, 0);
    hal_atomic_set(&done, 0);
    hal_atomic_set(&process_calls, 0);
    const eaf_thread_options_t scheduling = {EAF_THREAD_AUDIO, audio_cpu, false};
    rc = hal_thread_create_with_options(&audio, consume, NULL, &scheduling);
    if (rc) {
        output_log("Output setup failed rc=%d", rc);
        int cleanup = eaf_pipeline_deinit(&pipeline);
        if (cleanup) {
            active = true;
            hal_atomic_set(&failed, 1);
        }
        return cleanup ? cleanup : rc;
    }
    active = true;
    rc = eaf_pipeline_start(&pipeline);
    if (rc) {
        eaf_board_output_stop(); /* quit releases a still-gated worker. */
        return rc;
    }
    hal_atomic_set(&run_gate, held ? 0u : 1u);
    output_log("Stream: %u Hz, %u channels", format->sample_rate, (unsigned)format->num_channels);
    return 0;
}
int eaf_board_output_replace(const eaf_format_t *format, uint32_t ready_frames, bool held) {
    if (!format || !eaf_format_valid(format) || format->num_channels > 2)
        return EAF_UNSUPPORTED;
    eaf_board_output_stop();
    if (active)
        return EAF_IO; /* Teardown could not establish quiescence. */
    return eaf_board_output_start(format, ready_frames, held);
}
int eaf_board_output_release(void) {
    if (!active || hal_atomic_get(&failed))
        return EAF_STATE;
    hal_atomic_set(&pause_request, 0);
    hal_atomic_set(&pause_ack, 0);
    hal_atomic_set(&run_gate, 1);
    output_log("Output released after prefill");
    return EAF_OK;
}
uint32_t eaf_board_output_write(const int32_t *samples, uint32_t frames) {
    if (input_channels == 2)
        return eaf_reservoir_write(&reservoir, samples, frames);
    int32_t stereo[128u * 2u];
    if (frames > 128)
        frames = 128;
    for (size_t i = 0; i < frames; ++i)
        stereo[2u * i] = stereo[2u * i + 1u] = samples[i];
    return eaf_reservoir_write(&reservoir, stereo, frames);
}
void eaf_board_output_finish(void) {
    eaf_reservoir_finish(&reservoir);
    output_log("Input EOF");
}

int eaf_board_output_init(const eaf_board_output_config_t *config) {
    if (!config)
        return EAF_INVALID;
    if (!config->storage || config->capacity_frames < 2u ||
        (config->capacity_frames & (config->capacity_frames - 1u)))
        return EAF_INVALID;
    storage = config->storage;
    capacity = config->capacity_frames;
    log_sink = config->log;
    audio_cpu = config->audio_cpu;
    sink = board_sink();
    return board_sink_init();
}

int eaf_board_output_pause(bool paused) {
    return pause_output(paused);
}
int eaf_board_output_volume(int32_t left, int32_t right) {
    return set_volume(NULL, left, right);
}

bool eaf_board_output_failed(void) {
    return hal_atomic_get(&failed) != 0;
}
bool eaf_board_output_snapshot(eaf_board_playback_t *playback) {
    if (!active)
        return false;
    *playback =
        (eaf_board_playback_t){hal_atomic_get(&elapsed), capacity * 8u,
                               eaf_reservoir_level(&reservoir) * 8u, hal_atomic_get(&played) != 0};
    return true;
}
uint32_t eaf_board_output_capacity_frames(void) {
    return capacity;
}
uint32_t eaf_board_output_level(void) {
    return eaf_reservoir_level(&reservoir);
}
uint32_t eaf_board_output_underruns(void) {
    return hal_atomic_get(&underruns);
}
uint32_t eaf_board_output_queue_min(void) {
    uint32_t level = hal_atomic_get(&queue_min);
    return level == UINT32_MAX ? 0 : level;
}
uint32_t eaf_board_output_process_calls(void) {
    return hal_atomic_get(&process_calls);
}
uint32_t eaf_board_output_flags(void) {
    return (active ? 1u : 0u) | (hal_atomic_get(&run_gate) ? 2u : 0u) |
           (hal_atomic_get(&pause_request) ? 4u : 0u) | (hal_atomic_get(&pause_ack) ? 8u : 0u) |
           (hal_atomic_get(&done) ? 16u : 0u);
}
