#include "check.h"
#include <eaf/eaf_hal.h>
#include <eaf/eaf_lms_client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static eaf_lms_client_t client;
static uint32_t count;
static unsigned starts, stops;
static bool ended;
static unsigned volumes;
static int set_volume(void *ctx, int32_t left, int32_t right) {
    (void)ctx;
    static const int32_t expected[5][2] = {{INT32_MAX, INT32_MAX},
                                           {1073741824, 0},
                                           {INT32_MAX, 32768},
                                           {INT32_MAX, INT32_MAX},
                                           {536870912, 1073741824}};
    CHECK(volumes < 5);
    CHECK(left == expected[volumes][0] && right == expected[volumes][1]);
    ++volumes;
    return 0;
}
static unsigned pauses, resumes;
static int pause_output(void *ctx, bool paused) {
    (void)ctx;
    if (paused)
        ++pauses;
    else
        ++resumes;
    return 0;
}
static int start(void *ctx, const eaf_format_t *format) {
    (void)ctx;
    CHECK(format->sample_rate == 48000 && format->num_channels == 2);
    ++starts;
    return 0;
}
static uint32_t pcm(void *ctx, const int32_t *samples, uint32_t frames) {
    (void)ctx;
    CHECK(!client.wait_cont && !client.wait_start);
    if (frames > 3)
        frames = 3; /* Force partial writes/backpressure every iteration. */
    for (uint32_t i = 0; i < frames * 2u; ++i) {
        int32_t expected = ((int32_t)((count * 2u + i) % 101u) - 50) * 65536;
        CHECK(samples[i] == expected);
    }
    count += frames;
    return frames;
}
static void eof(void *ctx) {
    (void)ctx;
    ended = true;
}
static void stop(void *ctx) {
    (void)ctx;
    ++stops;
}
int main(int argc, char **argv) {
    CHECK(argc == 3);
    unsigned long port = strtoul(argv[1], NULL, 10);
    CHECK(port > 0 && port <= 65535);
    bool failure = !strcmp(argv[2], "fail");
    eaf_lms_callbacks_t cb = {.start = start,
                              .pcm = pcm,
                              .eof = eof,
                              .stop = stop,
                              .pause = pause_output,
                              .volume = set_volume};
    CHECK(eaf_lms_client_init(&client, &cb) == 0);
    const uint8_t mac[] = {2, 0, 0, 0, 0, 1};
    CHECK(eaf_lms_client_connect(&client, 0x7f000001u, (uint16_t)port, mac) == 0);
    int rc = 0;
    bool reported = false;
    uint64_t deadline = hal_monotonic_time_us() + 10000000u;
    while (!rc && (!ended || client.tx_sent < client.tx_used)) {
        CHECK(hal_monotonic_time_us() < deadline);
        rc = eaf_lms_client_step(&client);
        hal_sleep_ms(1);
        if (!rc && count && !reported) {
            /* Simulated output owner; acceptance is NOT a production output clock. */
            eaf_lms_playback_t playback = {1234, 4096, 512, true};
            CHECK(eaf_lms_client_report_playback(&client, &playback) == 0);
            CHECK(eaf_lms_client_report_playback(&client, &playback) == 0);
            reported = true;
        }
    }
    if (failure)
        CHECK(rc != 0 && !ended);
    else
        CHECK(!rc && ended && count == 1025 && starts == 1 && pauses == 1 && resumes == 1 &&
              volumes == 5);
    eaf_lms_client_close(&client);
    CHECK(stops == starts);
    puts("LMS peer PASS");
    return 0;
}
