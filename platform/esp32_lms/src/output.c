#include "output.h"
#include "diagnostics.h"
#include <eaf/eaf_core.h>
#include <eaf/eaf_dsp.h>
#include <stdio.h>
#define CAPACITY 4096u
static eaf_reservoir_t reservoir;
static eaf_pipeline_t pipeline;
static int32_t storage[CAPACITY * 2u];
static eaf_sink_t *sink;
static uint8_t input_channels;
static eaf_volume_ctx_t volume = {{INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX}};
static eaf_node_t master = {"LMS volume", EAF_NODE_STAGE_POST_PROCESS, &eaf_volume_ops, &volume};
/* Fixed -18 dB bench attenuation even if LMS requests full volume. */
static eaf_volume_ctx_t attenuation = {{268435456, 268435456, 268435456, 268435456}};
static eaf_node_t limiter = {"bench attenuation", EAF_NODE_STAGE_PRE_PROCESS, &eaf_volume_ops,
                             &attenuation};
static eaf_node_t *const nodes[] = {&limiter, &master};
/* All three atomics are sequentially consistent: one bounded coherent read per block.
   A concurrent update is retried next block, never spun on in the audio task. */
static atomic_uint gain_sequence, gain_left = INT32_MAX, gain_right = INT32_MAX;
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
static eaf_thread_t audio;
static eaf_atomic_u32_t quit, elapsed, played, failed, pause_request, pause_ack, done, run_gate;
static bool active;
static eaf_atomic_u32_t underruns, queue_min;
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
        if (rc && rc != EAF_EOF) {
            board_log("Audio process failed rc=%d state=%d", rc, (int)pipeline.state);
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
static int pause_output(void *ctx, bool paused) {
    (void)ctx;
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
static void stop(void *ctx) {
    (void)ctx;
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
    board_log("Stopped: source frames=%llu, underruns=%u",
              (unsigned long long)reservoir.frames_read, reservoir.underruns);
    if (eaf_pipeline_deinit(&pipeline)) {
        hal_atomic_set(&failed, 1);
        return;
    }
    active = false;
}
static int start_common(void *ctx, const eaf_format_t *fmt, uint32_t watermark, bool held) {
    (void)ctx;
    if (active || audio.impl || pipeline.state != EAF_UNINITIALIZED)
        return EAF_STATE;
    if (!fmt || !eaf_format_valid(fmt) || fmt->num_channels > 2)
        return EAF_UNSUPPORTED;
    board_log("Output request: %u Hz channels=%u", fmt->sample_rate, (unsigned)fmt->num_channels);
    input_channels = fmt->num_channels;
    eaf_pipeline_config_t config = {&reservoir, nodes, 2, sink};
    int rc = eaf_reservoir_init(&reservoir, storage, CAPACITY,
                                (eaf_format_t){fmt->sample_rate, 2, 3}, watermark);
    if (!rc)
        rc = eaf_pipeline_init(&pipeline, &config);
    if (!rc)
        rc = eaf_pipeline_configure(&pipeline, 128);
    if (rc) {
        board_log("Output setup failed rc=%d", rc);
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
    const eaf_thread_options_t scheduling = {EAF_THREAD_AUDIO, -1, false};
    rc = hal_thread_create_with_options(&audio, consume, NULL, &scheduling);
    if (rc) {
        board_log("Output setup failed rc=%d", rc);
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
        stop(NULL); /* quit releases a still-gated worker; no PCM can be processed. */
        return rc;
    }
    hal_atomic_set(&run_gate, held ? 0u : 1u);
    board_log("Stream: %u Hz, %u channels", fmt->sample_rate, (unsigned)fmt->num_channels);
    return 0;
}
static int start(void *ctx, const eaf_format_t *fmt) {
    return start_common(ctx, fmt, CAPACITY * 3u / 4u, false);
}
static int start_buffered(void *ctx, const eaf_format_t *fmt,
                          const eaf_lms_buffer_request_t *request,
                          eaf_lms_buffer_limits_t *limits) {
    int rc = eaf_lms_buffer_limits(fmt, request, CAPACITY, CAPACITY * 3u / 4u, limits);
    if (rc)
        return rc;
    board_log("Buffer request=%u B/%u ms ready=%u/%u frames clamped=%u", request->stream_bytes,
              request->output_ms, limits->ready_frames, limits->capacity_frames,
              limits->clamped ? 1u : 0u);
    return start_common(ctx, fmt, limits->ready_frames, true);
}
static int release_output(void *ctx) {
    (void)ctx;
    if (!active || hal_atomic_get(&failed))
        return EAF_STATE;
    hal_atomic_set(&pause_request, 0);
    hal_atomic_set(&pause_ack, 0);
    hal_atomic_set(&run_gate, 1);
    board_log("Output released after prefill");
    return EAF_OK;
}
static uint32_t pcm(void *ctx, const int32_t *samples, uint32_t frames) {
    (void)ctx;
    if (input_channels == 2)
        return eaf_reservoir_write(&reservoir, samples, frames);
    int32_t stereo[128u * 2u];
    if (frames > 128)
        frames = 128;
    for (size_t i = 0; i < frames; ++i)
        stereo[2u * i] = stereo[2u * i + 1u] = samples[i];
    return eaf_reservoir_write(&reservoir, stereo, frames);
}
static void eof(void *ctx) {
    (void)ctx;
    eaf_reservoir_finish(&reservoir);
    board_log("Input EOF");
}

int board_output_init(void) {
    sink = board_sink();
    return board_sink_init();
}
eaf_lms_callbacks_t board_output_callbacks(void) {
    return (eaf_lms_callbacks_t){.start = start,
                                 .pcm = pcm,
                                 .eof = eof,
                                 .stop = stop,
                                 .pause = pause_output,
                                 .volume = set_volume,
                                 .start_buffered = start_buffered,
                                 .release = release_output};
}
bool board_output_failed(void) {
    return hal_atomic_get(&failed) != 0;
}
bool board_output_snapshot(eaf_lms_playback_t *snapshot) {
    if (!active)
        return false;
    *snapshot =
        (eaf_lms_playback_t){hal_atomic_get(&elapsed), CAPACITY * 8u,
                             eaf_reservoir_level(&reservoir) * 8u, hal_atomic_get(&played) != 0};
    return true;
}

uint32_t board_output_underruns(void) {
    return hal_atomic_get(&underruns);
}

uint32_t board_output_queue_min(void) {
    uint32_t level = hal_atomic_get(&queue_min);
    return level == UINT32_MAX ? 0 : level;
}
