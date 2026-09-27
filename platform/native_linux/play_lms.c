/* LMS Linux player with timed null or optional ALSA output. */
#include "native_output.h"
#include <arpa/inet.h>
#include <eaf/eaf_core.h>
#include <eaf/eaf_dsp.h>
#include <eaf/eaf_lms_client.h>
#include <eaf/eaf_sink_null.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../esp32_output/board_output.c"
#define CAPACITY 16384u
static eaf_lms_client_t client;
static int32_t lms_storage[CAPACITY * 2u];
static eaf_lms_packet_fn dispatch;
static bool owner_ready;

static eaf_sink_t *lms_sink(void) {
    return &sink;
}
static int lms_sink_pause(bool paused) {
    return eaf_native_output_pause(&sink, paused);
}
static uint32_t lms_delay(void) {
    return eaf_native_output_delay(&sink);
}
static int start(void *ctx, const eaf_format_t *fmt) {
    (void)ctx;
    if (!owner_ready) {
        eaf_board_output_config_t config = {.audio_cpu = -1,
                                            .log = NULL,
                                            .storage = lms_storage,
                                            .capacity_frames = CAPACITY,
                                            .sink = lms_sink,
                                            .sink_init = NULL,
                                            .sink_pause = lms_sink_pause,
                                            .delay = lms_delay};
        int rc = eaf_board_output_init(&config);
        if (rc)
            return rc;
        owner_ready = true;
    }
    return eaf_board_output_start(fmt, CAPACITY * 3u / 4u, false);
}
static void stop(void *ctx) {
    (void)ctx;
    eaf_board_output_stop();
}
static uint32_t pcm(void *ctx, const int32_t *samples, uint32_t frames) {
    (void)ctx;
    return eaf_board_output_write(samples, frames);
}
static void eof(void *ctx) {
    (void)ctx;
    eaf_board_output_finish();
}
static int pause_output(void *ctx, bool paused) {
    (void)ctx;
    return eaf_board_output_pause(paused);
}
static int lms_volume(void *ctx, int32_t left, int32_t right) {
    (void)ctx;
    return eaf_board_output_volume(left, right);
}
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
int main(int argc, char **argv) {
    struct in_addr address;
    char *end = NULL;
    unsigned long seconds = argc >= 3 ? strtoul(argv[2], &end, 10) : 60;
    if (argc < 2 || argc > 4 || inet_pton(AF_INET, argv[1], &address) != 1 || !seconds ||
        seconds > 86400 || (argc >= 3 && (!*argv[2] || *end))) {
        fprintf(stderr, "Usage: %s SERVER_IPV4 [seconds: 1..86400] [ALSA_DEVICE]\n", argv[0]);
        return 2;
    }
    if (eaf_native_output_select(&sink, argc == 4 ? argv[3] : NULL)) {
        fprintf(stderr, "Requested output is unavailable in this build\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    eaf_lms_callbacks_t cb = {.start = start,
                              .pcm = pcm,
                              .eof = eof,
                              .stop = stop,
                              .pause = pause_output,
                              .volume = lms_volume};
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
