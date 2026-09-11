/* LMS Linux player with timed null or optional ALSA output. */
#include <arpa/inet.h>
#include <eaf/eaf_core.h>
#include <eaf/eaf_dsp.h>
#ifdef EAF_HAVE_ALSA
#include <eaf/eaf_sink_alsa.h>
#endif
#include <eaf/eaf_lms_client.h>
#include <eaf/eaf_sink_null.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CAPACITY 16384u
static eaf_lms_client_t client;
static eaf_reservoir_t reservoir;
static eaf_pipeline_t pipeline;
static int32_t storage[CAPACITY * 2u];
static eaf_null_sink_ctx_t sink_ctx;
static eaf_sink_t sink = {&eaf_null_sink_ops, &sink_ctx};
#ifdef EAF_HAVE_ALSA
static eaf_alsa_sink_ctx_t alsa;
static bool use_alsa;
#endif
static eaf_volume_ctx_t volume = {{INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX}};
static eaf_node_t master = {"LMS volume", EAF_NODE_STAGE_POST_PROCESS, &eaf_volume_ops, &volume};
static eaf_node_t *const nodes[] = {&master};
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
static eaf_lms_packet_fn dispatch;
static int trace(void *ctx, const uint8_t *p, size_t n) {
    printf("Control %.4s (%zu bytes)", (const char *)p, n);
    if (n >= 11u && !memcmp(p, "strm", 4))
        printf(" cmd=%c auto=%c codec=%c PCM=%c/%c/%c/%c", p[4], p[5], p[6], p[7], p[8], p[9],
               p[10]);
    putchar('\n');
    return dispatch(ctx, p, n);
}
static volatile sig_atomic_t interrupted;
static void on_signal(int sig) {
    (void)sig;
    interrupted = 1;
}
static void consume(void *ctx) {
    (void)ctx;
    /* No graph access until START has reset the reservoir and enabled output. */
    while (!hal_atomic_get(&run_gate) && !hal_atomic_get(&quit))
        hal_sleep_ms(1);
    while (!hal_atomic_get(&quit)) {
        if (hal_atomic_get(&pause_request)) {
            uint64_t before = hal_monotonic_time_us();
#ifdef EAF_HAVE_ALSA
            if (use_alsa && eaf_alsa_pause(&alsa, true)) {
                hal_atomic_set(&failed, 1);
                break;
            }
#endif
            hal_atomic_set(&pause_ack, 1);
            while (hal_atomic_get(&pause_request) && !hal_atomic_get(&quit))
                hal_sleep_ms(1);
#ifdef EAF_HAVE_ALSA
            if (use_alsa) {
                if (eaf_alsa_pause(&alsa, false)) {
                    hal_atomic_set(&failed, 1);
                    break;
                }
            } else
#endif
                sink_ctx.epoch_us += hal_monotonic_time_us() - before;
            hal_atomic_set(&pause_ack, 0);
            continue;
        }
        apply_volume();
        int rc = eaf_pipeline_process(&pipeline);
        if (rc && rc != EAF_EOF) {
            hal_atomic_set(&failed, 1);
            break;
        }
        uint64_t presented = reservoir.frames_read;
#ifdef EAF_HAVE_ALSA
        if (use_alsa) {
            uint32_t delay = eaf_alsa_delay(&alsa);
            presented = presented > delay ? presented - delay : 0;
        }
#endif
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
    printf("Stopped: source frames=%llu, underruns=%u\n", (unsigned long long)reservoir.frames_read,
           reservoir.underruns);
    if (eaf_pipeline_deinit(&pipeline)) {
        hal_atomic_set(&failed, 1);
        return;
    }
    active = false;
}
static int start(void *ctx, const eaf_format_t *fmt) {
    (void)ctx;
    if (active || audio.impl || pipeline.state != EAF_UNINITIALIZED)
        return EAF_STATE;
    eaf_pipeline_config_t config = {&reservoir, nodes, 1, &sink};
    int rc = eaf_reservoir_init(&reservoir, storage, CAPACITY, *fmt, 4096);
    if (!rc)
        rc = eaf_pipeline_init(&pipeline, &config);
    if (!rc)
        rc = eaf_pipeline_configure(&pipeline, 128);
    if (rc) {
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
    hal_atomic_set(&played, 0);
    hal_atomic_set(&failed, 0);
    hal_atomic_set(&pause_request, 0);
    hal_atomic_set(&pause_ack, 0);
    hal_atomic_set(&done, 0);
    rc = hal_thread_create(&audio, consume, NULL);
    if (rc) {
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
    hal_atomic_set(&run_gate, 1);
    printf("Stream: %u Hz, %u channels\n", fmt->sample_rate, (unsigned)fmt->num_channels);
    return 0;
}
static uint32_t pcm(void *ctx, const int32_t *samples, uint32_t frames) {
    (void)ctx;
    return eaf_reservoir_write(&reservoir, samples, frames);
}
static void eof(void *ctx) {
    (void)ctx;
    eaf_reservoir_finish(&reservoir);
    puts("Input EOF");
}
int main(int argc, char **argv) {
    struct in_addr address;
    char *end = NULL;
    unsigned long seconds = argc >= 3 ? strtoul(argv[2], &end, 10) : 60;
    if (argc < 2 || argc > 4 || inet_pton(AF_INET, argv[1], &address) != 1 || !seconds ||
        seconds > 86400 || (argc >= 3 && (!*argv[2] || *end))) {
        fprintf(stderr, "Usage: %s SERVER_IPV4 [seconds: 1..86400] [ALSA_DEVICE]\n", argv[0]);
        return 2;
    }
    if (argc == 4) {
#ifdef EAF_HAVE_ALSA
        use_alsa = true;
        alsa.device = argv[3];
        sink = (eaf_sink_t){&eaf_alsa_sink_ops, &alsa};
#else
        fprintf(stderr, "ALSA support was not built\n");
        return 2;
#endif
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    eaf_lms_callbacks_t cb = {.start = start,
                              .pcm = pcm,
                              .eof = eof,
                              .stop = stop,
                              .pause = pause_output,
                              .volume = set_volume};
    const uint8_t mac[] = {2, 0xea, 0xf0, 0, 0, 1};
    int rc = eaf_lms_client_init(&client, &cb);
    if (!rc)
        rc = eaf_lms_client_connect(&client, ntohl(address.s_addr), 3483, mac);
    uint64_t deadline = hal_monotonic_time_us() + seconds * UINT64_C(1000000), report = 0;
    if (!rc) {
        dispatch = client.parser.on_packet;
        client.parser.on_packet = trace;
    }
    if (!rc)
        printf("Connected: player 02:ea:f0:00:00:01; output=%s\n",
               argc == 4 ? argv[3] : "timed null (inaudible)");
    while (!rc && !interrupted && hal_monotonic_time_us() < deadline) {
        rc = eaf_lms_client_step(&client);
        if (!rc && hal_atomic_get(&failed))
            rc = EAF_IO;
        uint64_t now = hal_monotonic_time_us();
        if (!rc && active && !client.wait_cont && !client.wait_start && now >= report) {
            eaf_lms_playback_t p = {
                hal_atomic_get(&elapsed), CAPACITY * reservoir.format.num_channels * 4u,
                eaf_reservoir_level(&reservoir) * reservoir.format.num_channels * 4u,
                hal_atomic_get(&played) != 0};
            rc = eaf_lms_client_report_playback(&client, &p);
            report = now + 1000000u;
        }
        hal_sleep_ms(1);
    }
    eaf_lms_client_close(&client);
    if (!rc && hal_atomic_get(&failed))
        rc = EAF_IO;
    if (rc)
        fprintf(stderr, "LMS error: %d\n", rc);
    return rc ? 1 : 0;
}
