#include "check.h"
#include <eaf/eaf_hal.h>
#include <eaf/eaf_lms_client.h>
#include <string.h>
static eaf_lms_client_t client;
static uint8_t wire[65536], control[30];
static size_t wire_size, offset, control_size;
static uint32_t accepted, accept_limit, fragment;
static unsigned channels, reads, controls, stops;
static uint64_t now;
static int http_error;
static bool ended;
uint64_t __wrap_hal_monotonic_time_us(void) {
    return now;
}
int __wrap_hal_tcp_recv(eaf_tcp_t *tcp, void *data, size_t capacity, size_t *received) {
    *received = 0;
    now += 10;
    if (tcp->fd == 1) {
        ++controls;
        if (!control_size)
            return EAF_AGAIN;
        CHECK(control_size <= capacity);
        memcpy(data, control, control_size);
        *received = control_size;
        control_size = 0;
        return EAF_OK;
    }
    ++reads;
    if (http_error)
        return http_error;
    if (offset == wire_size)
        return EAF_EOF;
    size_t n = wire_size - offset;
    if (n > capacity)
        n = capacity;
    if (n > fragment)
        n = fragment;
    memcpy(data, wire + offset, n);
    offset += n;
    *received = n;
    return EAF_OK;
}
int __wrap_hal_tcp_send(eaf_tcp_t *tcp, const void *data, size_t length, size_t *sent) {
    (void)tcp;
    (void)data;
    *sent = length > 7 ? 7 : length; /* Fragment outgoing control writes. */
    return EAF_OK;
}
void __wrap_hal_tcp_close(eaf_tcp_t *tcp) {
    tcp->open = false;
}
static int start(void *ctx, const eaf_format_t *fmt) {
    (void)ctx;
    (void)fmt;
    return 0;
}
static uint32_t pcm(void *ctx, const int32_t *samples, uint32_t frames) {
    (void)ctx;
    if (frames > accept_limit)
        frames = accept_limit;
    for (size_t i = 0; i < (size_t)frames * channels; ++i) {
        int32_t expected = ((int32_t)(((size_t)accepted * channels + i) % 101u) - 50) * 65536;
        CHECK(samples[i] == expected);
    }
    accepted += frames;
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
static void setup(unsigned width, unsigned channel_count, uint32_t rate) {
    eaf_lms_callbacks_t cb = {.start = start, .pcm = pcm, .eof = eof, .stop = stop};
    CHECK(!eaf_lms_client_init(&client, &cb));
    client.control = (eaf_tcp_t){1, true};
    client.http = (eaf_tcp_t){2, true};
    client.streaming = client.headers_done = true;
    client.width = width;
    channels = channel_count;
    client.format = (eaf_format_t){rate, (uint8_t)channels, channels == 2 ? 3 : 8};
    wire_size = (size_t)8192 * channels * width;
    for (size_t i = 0; i < wire_size / width; ++i) {
        int32_t sample = ((int32_t)(i % 101u) - 50) * 65536;
        uint32_t raw = (uint32_t)sample >> (32u - width * 8u);
        for (unsigned b = 0; b < width; ++b)
            wire[i * width + b] = (uint8_t)(raw >> (8u * b));
    }
    offset = control_size = 0;
    now = 0;
    accepted = reads = controls = stops = 0;
    accept_limit = 512;
    fragment = 1024;
    http_error = 0;
    ended = false;
}
int main(void) {
    eaf_lms_pump_result_t result;
    for (uint32_t rate = 44100; rate <= 96000; rate = rate == 44100 ? 48000 : rate * 2u) {
        for (unsigned width = 2; width <= 4; ++width) {
            for (unsigned ch = 1; ch <= 2; ++ch) {
                setup(width, ch, rate);
                CHECK(!eaf_lms_client_pump(&client, 16, 1000, &result));
                CHECK(result.budget_exhausted && reads == 16 && controls == 16);
                CHECK(client.diagnostics.http_bytes == 16384); /* More than one recv per wake. */
                unsigned batches = 0;
                fragment = 7; /* Misaligned sample/frame boundaries. */
                accept_limit = 3;
                while (!ended && ++batches < 20000)
                    CHECK(!eaf_lms_client_pump(&client, 32, 1000, &result));
                CHECK(ended && accepted == 8192 && client.diagnostics.pcm_frames == 8192);
                CHECK(offset == wire_size && controls >= reads);
                eaf_lms_client_close(&client);
                CHECK(stops == 1);
            }
        }
    }
    setup(4, 2, 96000);
    CHECK(!eaf_lms_client_pump(&client, 64, 30, &result));
    CHECK(result.steps == 2 && result.budget_exhausted); /* Time limits admission too. */
    setup(2, 2, 44100);
    accept_limit = 0;
    CHECK(!eaf_lms_client_pump(&client, 64, 1000, &result));
    CHECK(result.steps == 2 && result.backpressured && reads == 1);
    CHECK(!eaf_lms_client_pump(&client, 64, 1000, &result));
    CHECK(result.steps == 1 && !result.progressed && reads == 1);
    /* STOP still arrives while the PCM consumer is blocked. */
    memset(control, 0, sizeof(control));
    control[1] = 28;
    /* Wire opcode and command are not a C string. */
    // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
    memcpy(control + 2, "strmq", 5);
    control_size = sizeof(control);
    CHECK(!eaf_lms_client_pump(&client, 64, 1000, &result));
    CHECK(stops == 1 && !client.streaming);
    setup(2, 2, 44100);
    http_error = EAF_AGAIN;
    CHECK(!eaf_lms_client_pump(&client, 64, 1000, &result));
    CHECK(result.steps == 1 && !result.progressed && client.diagnostics.recv_again == 1);
    http_error = EAF_IO;
    CHECK(eaf_lms_client_pump(&client, 64, 1000, &result) == EAF_IO);
    CHECK(client.diagnostics.first_error == EAF_IO &&
          client.diagnostics.error_stage == EAF_LMS_STAGE_HTTP_RECV);
    eaf_lms_client_close(&client);
    CHECK(client.diagnostics.first_error == EAF_IO && stops == 1);
    CHECK(eaf_lms_client_pump(&client, 0, 1000, &result) == EAF_INVALID);
    return 0;
}
