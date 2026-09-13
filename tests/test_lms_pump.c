#include "check.h"
#include <eaf/eaf_hal.h>
#include <eaf/eaf_lms_client.h>
#include <stdio.h>
#include <string.h>
static eaf_lms_client_t client;
static uint8_t wire[65536], control[128];
static size_t wire_size, offset, control_size;
static uint32_t accepted, accept_limit, fragment;
static unsigned channels, reads, controls, stops;
static uint64_t now;
static int http_error;
static bool ended, held;
static unsigned releases;
static eaf_lms_buffer_limits_t limits;
static eaf_lms_buffer_request_t requested;
static char header[128];
static size_t header_size, header_offset;
static uint8_t sent_wire[4096];
static size_t sent_size;
int __wrap_hal_tcp_connect(eaf_tcp_t *tcp, uint32_t ip, uint16_t port, uint32_t timeout) {
    (void)ip;
    (void)port;
    (void)timeout;
    *tcp = (eaf_tcp_t){2, true};
    return EAF_OK;
}
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
    if (header_offset < header_size) {
        size_t n = header_size - header_offset;
        if (n > capacity)
            n = capacity;
        memcpy(data, header + header_offset, n);
        header_offset += n;
        *received = n;
        return EAF_OK;
    }
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
    *sent = length > 7 ? 7 : length; /* Fragment outgoing control writes. */
    if (tcp->fd == 1) {
        CHECK(sent_size + *sent <= sizeof(sent_wire));
        memcpy(sent_wire + sent_size, data, *sent);
        sent_size += *sent;
    }
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
    if (held && frames > limits.capacity_frames - accepted)
        frames = limits.capacity_frames - accepted;
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
    ended = held = false;
    releases = 0;
    header_size = header_offset = sent_size = 0;
}
static int buffered_start(void *ctx, const eaf_format_t *fmt,
                          const eaf_lms_buffer_request_t *request, eaf_lms_buffer_limits_t *out) {
    (void)ctx;
    requested = *request;
    CHECK(!eaf_lms_buffer_limits(fmt, request, 2048, 1024, out));
    limits = *out;
    held = true;
    return EAF_OK;
}
static int release(void *ctx) {
    (void)ctx;
    CHECK(held && (accepted >= limits.ready_frames || ended));
    CHECK(!client.wait_start && !client.wait_cont && !client.output_paused);
    held = false;
    ++releases;
    return EAF_OK;
}
static int pause_output(void *ctx, bool paused) {
    (void)ctx;
    (void)paused;
    return EAF_OK;
}
static void command_packet(char cmd) {
    memset(control, 0, sizeof(control));
    control[1] = 28;
    control[2] = 's';
    control[3] = 't';
    control[4] = 'r';
    control[5] = 'm';
    control[6] = (uint8_t)cmd;
    control_size = 30;
}
static unsigned event_count(const char *event) {
    unsigned count = 0;
    for (size_t i = 0; i + 8u <= sent_size;) {
        uint32_t n = (uint32_t)sent_wire[i + 4] << 24 | (uint32_t)sent_wire[i + 5] << 16 |
                     (uint32_t)sent_wire[i + 6] << 8 | sent_wire[i + 7];
        CHECK(n <= sent_size - i - 8u);
        if (!memcmp(sent_wire + i, "STAT", 4) && n >= 4 && !memcmp(sent_wire + i + 8u, event, 4))
            ++count;
        i += 8u + n;
    }
    return count;
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static unsigned tx_event_count(const char *event) {
    unsigned count = 0;
    for (size_t i = 0; i + 8u <= client.tx_used;) {
        uint32_t n = be32(client.tx + i + 4u);
        CHECK(n <= client.tx_used - i - 8u);
        if (!memcmp(client.tx + i, "STAT", 4) && n >= 4 && !memcmp(client.tx + i + 8u, event, 4))
            ++count;
        i += 8u + n;
    }
    return count;
}
static uint32_t tx_stat_word(const char *event, size_t body_offset) {
    for (size_t i = 0; i + 8u <= client.tx_used;) {
        uint32_t n = be32(client.tx + i + 4u);
        if (!memcmp(client.tx + i, "STAT", 4) && n >= body_offset + 4u &&
            !memcmp(client.tx + i + 8u, event, 4))
            return be32(client.tx + i + 8u + body_offset);
        i += 8u + n;
    }
    return 0;
}
static void buffering_case(unsigned autostart, unsigned frames, bool oversized, bool paused) {
    setup(3, 2, 48000);
    wire_size = (size_t)frames * 6u;
    client.streaming = false;
    client.http.open = false;
    client.callbacks.start_buffered = buffered_start;
    client.callbacks.release = release;
    client.callbacks.pause = pause_output;
    command_packet('s');
    control[7] = (uint8_t)('0' + autostart);
    control[8] = 'p';
    control[9] = '2';
    control[10] = '4';
    control[11] = '2';
    control[12] = '1';
    control[13] = oversized ? 255 : 1; /* Literal wire stream threshold at payload byte 11. */
    control[18] = oversized ? 255 : 0; /* Output threshold at payload byte 16. */
    control[24] = 0x23;
    control[25] = 0x28; /* 9000 */
    const char request[] = "GET /pcm HTTP/1.0\r\n\r\n";
    memcpy(control + 30, request, sizeof(request) - 1u);
    control_size += sizeof(request) - 1u;
    control[1] = (uint8_t)(control_size - 2u);
    header_size = (size_t)snprintf(header, sizeof(header),
                                   "HTTP/1.0 200 OK\r\nContent-Length: %zu\r\n\r\n", wire_size);
    eaf_lms_pump_result_t result;
    CHECK(!eaf_lms_client_pump(&client, 1, 1000, &result));
    CHECK(requested.stream_bytes == (oversized ? 261120u : 1024u));
    CHECK(requested.output_ms == (oversized ? 25500u : 0u));
    CHECK(limits.ready_frames == (oversized ? 2048u : 1024u));
    CHECK(limits.clamped == oversized);
    if (autostart >= 2) {
        CHECK(!eaf_lms_client_pump(&client, 32, 1000, &result));
        CHECK(!accepted && !releases && client.wait_cont && !client.ready_sent);
        memset(control, 0, sizeof(control));
        control[1] = 9;
        control[2] = 'c';
        control[3] = 'o';
        control[4] = 'n';
        control[5] = 't';
        control_size = 11;
        CHECK(!eaf_lms_client_pump(&client, 1, 1000, &result));
    }
    if (paused) {
        command_packet('p');
        CHECK(!eaf_lms_client_pump(&client, 1, 1000, &result));
        CHECK(client.output_paused && held);
    }
    for (unsigned i = 0; i < 100 && !client.ready_sent && !ended && !releases; ++i)
        CHECK(!eaf_lms_client_pump(&client, 32, 1000, &result));
    bool wait = autostart == 0 || autostart == 2;
    if (wait || paused) {
        CHECK(!releases && held);
        if (wait)
            CHECK(client.ready_sent);
        command_packet('u');
    }
    for (unsigned i = 0; i < 200 && (!ended || !releases || client.tx_sent < client.tx_used); ++i)
        CHECK(!eaf_lms_client_pump(&client, 32, 1000, &result));
    CHECK(ended && accepted == frames && releases == 1);
    CHECK(event_count("STMl") == (wait ? 1u : 0u));
    CHECK(event_count("STMd") == 1);
    for (unsigned i = 0; i < 3; ++i)
        CHECK(!eaf_lms_client_pump(&client, 32, 1000, &result));
    CHECK(event_count("STMd") == 1);
    eaf_lms_client_close(&client);
    CHECK(stops == 1);
}
int main(void) {
    eaf_lms_pump_result_t result;
    for (unsigned mode = 0; mode < 4; ++mode) {
        buffering_case(mode, 8192, false, false);
        buffering_case(mode, 8192, true, false);
        buffering_case(mode, 32, true, false);
        buffering_case(mode, 0, true, false);
    }
    buffering_case(1, 8192, false, true);
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
    /* The ingress ring keeps draining the socket while the output callback is
       backpressured, until the ring itself fills. */
    CHECK(result.backpressured && reads > 1);
    unsigned filled = reads;
    CHECK(!eaf_lms_client_pump(&client, 64, 1000, &result));
    CHECK(result.steps == 1 && !result.progressed && reads == filled);
    /* STOP still arrives while the PCM consumer is blocked. */
    memset(control, 0, sizeof(control));
    control[1] = 28;
    /* Wire opcode and command are not a C string. */
    // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
    memcpy(control + 2, "strmq", 5);
    control_size = 30;
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
    /* Output lifecycle: STMo on starvation, STMu after EOF drain, each once. */
    setup(2, 2, 44100);
    eaf_lms_playback_t pb = {1000, 4096, 512, true};
    CHECK(!eaf_lms_client_report_playback(&client, &pb));
    CHECK(tx_event_count("STMs") == 1 && tx_event_count("STMo") == 0);
    CHECK(tx_stat_word("STMs", 7) == EAF_LMS_INGRESS_BYTES); /* stream_buffer_size */
    pb.queued_bytes = 0;
    CHECK(!eaf_lms_client_report_playback(&client, &pb));
    CHECK(tx_event_count("STMo") == 1);
    CHECK(!eaf_lms_client_report_playback(&client, &pb));
    CHECK(tx_event_count("STMo") == 1); /* no duplicate without a refill */
    pb.queued_bytes = 512;
    CHECK(!eaf_lms_client_report_playback(&client, &pb)); /* re-arm */
    pb.queued_bytes = 0;
    client.input_eof = true;
    CHECK(!eaf_lms_client_report_playback(&client, &pb));
    CHECK(tx_event_count("STMu") == 1);
    CHECK(!eaf_lms_client_report_playback(&client, &pb));
    CHECK(tx_event_count("STMu") == 1);
    return 0;
}
