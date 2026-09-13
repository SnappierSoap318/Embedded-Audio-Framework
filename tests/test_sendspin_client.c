#include "check.h"
#include <eaf/eaf_hal.h>
#include <eaf/eaf_sendspin_client.h>
#include <stdio.h>
#include <string.h>

static eaf_sendspin_client_t client;
static uint8_t sent_wire[8192];
static size_t sent_size;
static uint64_t now = 1000000;
static unsigned ready_count, start_count, audio_count, end_count, clear_count, command_count,
    disconnect_count;
static int64_t audio_timestamp;
static uint8_t audio_bytes[64];
static size_t audio_length;

static const char http_response[] = "HTTP/1.1 101 Switching Protocols\r\n"
                                    "Upgrade: websocket\r\nConnection: upgrade\r\n\r\n";
static size_t http_offset;
static uint8_t server_stream[4096];
static size_t server_size, server_offset;
static size_t recv_fragment = 7;

int __wrap_hal_tcp_connect(eaf_tcp_t *tcp, uint32_t ip, uint16_t port, uint32_t timeout) {
    (void)ip;
    (void)port;
    (void)timeout;
    *tcp = (eaf_tcp_t){2, true};
    return EAF_OK;
}

uint64_t __wrap_hal_monotonic_time_us(void) {
    now += 1000;
    return now;
}

int __wrap_hal_tcp_send(eaf_tcp_t *tcp, const void *data, size_t length, size_t *sent) {
    (void)tcp;
    *sent = length > 7u ? 7u : length;
    CHECK(sent_size + *sent <= sizeof(sent_wire));
    memcpy(sent_wire + sent_size, data, *sent);
    sent_size += *sent;
    return EAF_OK;
}

int __wrap_hal_tcp_recv(eaf_tcp_t *tcp, void *data, size_t capacity, size_t *received) {
    (void)tcp;
    *received = 0;
    if (client.state == EAF_SENDPIN_CLIENT_HTTP) {
        size_t remaining = sizeof(http_response) - 1u - http_offset;
        if (!remaining)
            return EAF_AGAIN;
        size_t take = remaining > capacity ? capacity : remaining;
        memcpy(data, http_response + http_offset, take);
        http_offset += take;
        *received = take;
        return EAF_OK;
    }
    if (server_offset == server_size)
        return EAF_EOF;
    size_t take = server_size - server_offset;
    if (take > capacity)
        take = capacity;
    if (take > recv_fragment)
        take = recv_fragment;
    memcpy(data, server_stream + server_offset, take);
    server_offset += take;
    *received = take;
    return EAF_OK;
}

void __wrap_hal_tcp_close(eaf_tcp_t *tcp) {
    tcp->open = false;
}

static void on_ready(void *ctx, const eaf_sendspin_server_hello_t *hello) {
    (void)ctx;
    CHECK(hello->player_active && hello->version == 1);
    ++ready_count;
}
static void on_start(void *ctx, const eaf_sendspin_stream_start_t *start) {
    (void)ctx;
    CHECK(start->codec == EAF_SENDPIN_CODEC_PCM && start->sample_rate == 44100);
    ++start_count;
}
static void on_audio(void *ctx, const eaf_sendspin_stream_start_t *format, int64_t timestamp,
                     const uint8_t *pcm, size_t length) {
    (void)ctx;
    CHECK(format->codec == EAF_SENDPIN_CODEC_PCM);
    audio_timestamp = timestamp;
    audio_length = length;
    CHECK(length <= sizeof(audio_bytes));
    memcpy(audio_bytes, pcm, length);
    ++audio_count;
}
static void on_end(void *ctx, bool player) {
    (void)ctx;
    CHECK(player);
    ++end_count;
}
static void on_clear(void *ctx, bool player) {
    (void)ctx;
    CHECK(player);
    ++clear_count;
}
static void on_command(void *ctx, const eaf_sendspin_server_command_t *command) {
    (void)ctx;
    CHECK(command->command == EAF_SENDPIN_COMMAND_VOLUME && command->volume == 7);
    ++command_count;
}
static void on_disconnect(void *ctx) {
    (void)ctx;
    ++disconnect_count;
}

static size_t append_frame(size_t offset, uint8_t opcode, const uint8_t *payload, size_t length) {
    server_stream[offset] = (uint8_t)(0x80u | opcode);
    size_t position = 2;
    if (length <= 125u) {
        server_stream[offset + 1] = (uint8_t)length;
    } else if (length <= 0xFFFFu) {
        server_stream[offset + 1] = 126;
        server_stream[offset + 2] = (uint8_t)(length >> 8);
        server_stream[offset + 3] = (uint8_t)length;
        position = 4;
    } else {
        server_stream[offset + 1] = 127;
        for (size_t i = 0; i < 8; ++i)
            server_stream[offset + 2 + i] = (uint8_t)((uint64_t)length >> (56u - 8u * i));
        position = 10;
    }
    memcpy(server_stream + offset + position, payload, length);
    return offset + position + length;
}

static void script_server(void) {
    static const char hello[] =
        "{\"payload\":{\"server_id\":\"abc\",\"name\":\"MA\",\"version\":1,"
        "\"connection_reason\":\"discovery\",\"active_roles\":[\"player@v1\"]},"
        "\"type\":\"server/hello\"}";
    static const char time1[] =
        "{\"payload\":{\"client_transmitted\":1000000,\"server_received\":1000200,"
        "\"server_transmitted\":1000300},\"type\":\"server/time\"}";
    static const char time2[] =
        "{\"payload\":{\"client_transmitted\":1005000,\"server_received\":1005200,"
        "\"server_transmitted\":1005300},\"type\":\"server/time\"}";
    static const char start[] =
        "{\"payload\":{\"server_transmitted\":1000600,\"player\":{\"codec\":\"pcm\","
        "\"sample_rate\":44100,\"channels\":2,\"bit_depth\":16}},\"type\":\"stream/start\"}";
    static const char command[] = "{\"payload\":{\"player\":{\"command\":\"volume\",\"volume\":7}},"
                                  "\"type\":\"server/command\"}";
    static const char clear[] = "{\"payload\":{\"roles\":[\"player\"]},\"type\":\"stream/clear\"}";
    static const char end[] =
        "{\"payload\":{\"server_transmitted\":1001000,\"roles\":[\"player\"]},"
        "\"type\":\"stream/end\"}";
    size_t offset = 0;
    offset = append_frame(offset, EAF_SENDPIN_WS_TEXT, (const uint8_t *)hello, sizeof(hello) - 1u);
    offset = append_frame(offset, EAF_SENDPIN_WS_TEXT, (const uint8_t *)time1, sizeof(time1) - 1u);
    offset = append_frame(offset, EAF_SENDPIN_WS_TEXT, (const uint8_t *)time2, sizeof(time2) - 1u);
    offset = append_frame(offset, EAF_SENDPIN_WS_TEXT, (const uint8_t *)start, sizeof(start) - 1u);
    offset =
        append_frame(offset, EAF_SENDPIN_WS_TEXT, (const uint8_t *)command, sizeof(command) - 1u);
    uint8_t audio[9 + 16];
    audio[0] = 0x04u;
    int64_t timestamp = 123456789;
    for (size_t i = 0; i < 8; ++i)
        audio[1 + i] = (uint8_t)((uint64_t)timestamp >> (56u - 8u * i));
    for (size_t i = 0; i < 16; ++i)
        audio[9 + i] = (uint8_t)(i + 1u);
    offset = append_frame(offset, EAF_SENDPIN_WS_BINARY, audio, sizeof(audio));
    offset = append_frame(offset, EAF_SENDPIN_WS_TEXT, (const uint8_t *)clear, sizeof(clear) - 1u);
    offset = append_frame(offset, EAF_SENDPIN_WS_TEXT, (const uint8_t *)end, sizeof(end) - 1u);
    server_size = offset;
}

static bool contains(const char *needle) {
    size_t n = strlen(needle);
    if (n > sent_size)
        return false;
    for (size_t i = 0; i + n <= sent_size; ++i) {
        if (!memcmp(sent_wire + i, needle, n))
            return true;
    }
    return false;
}

/* Decode the client's masked WebSocket frames and search the payloads. */
static bool decoded_contains(const char *needle) {
    size_t needle_length = strlen(needle);
    uint8_t payload[4096];
    size_t offset = 0;
    while (offset + 3u < sent_size) {
        if (!memcmp(sent_wire + offset, "\r\n\r\n", 4)) {
            offset += 4u;
            break;
        }
        ++offset;
    }
    while (offset + 2u <= sent_size) {
        uint8_t b1 = sent_wire[offset + 1];
        bool masked = (b1 & 0x80u) != 0;
        uint64_t length = b1 & 0x7Fu;
        size_t position = 2;
        if (length == 126u) {
            CHECK(offset + 4u <= sent_size);
            length = (uint64_t)sent_wire[offset + 2] << 8 | sent_wire[offset + 3];
            position = 4;
        } else if (length == 127u) {
            CHECK(offset + 10u <= sent_size);
            length = 0;
            for (size_t i = 0; i < 8; ++i)
                length = length << 8 | sent_wire[offset + 2 + i];
            position = 10;
        }
        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked) {
            CHECK(offset + position + 4u <= sent_size);
            memcpy(mask, sent_wire + offset + position, 4);
            position += 4;
        }
        if (offset + position + length > sent_size || length > sizeof(payload))
            return false;
        for (size_t i = 0; i < (size_t)length; ++i)
            payload[i] = sent_wire[offset + position + i] ^ (masked ? mask[i & 3u] : 0u);
        for (size_t i = 0; i + needle_length <= (size_t)length; ++i) {
            if (!memcmp(payload + i, needle, needle_length))
                return true;
        }
        offset += position + (size_t)length;
    }
    return false;
}

int main(void) {
    script_server();
    eaf_sendspin_config_t config = {.client_id = "eaf-test",
                                    .name = "EAF Test",
                                    .product_name = "EAF Test",
                                    .manufacturer = "EAF",
                                    .software_version = "test",
                                    .sample_rate = 44100,
                                    .channels = 2,
                                    .bit_depth = 16,
                                    .buffer_capacity = 65536,
                                    .support_volume = true,
                                    .support_mute = true,
                                    .volume = 100,
                                    .static_delay_ms = 0,
                                    .required_lead_time_ms = 250,
                                    .min_buffer_ms = 250};
    eaf_sendspin_callbacks_t callbacks = {.ready = on_ready,
                                          .stream_start = on_start,
                                          .audio = on_audio,
                                          .stream_end = on_end,
                                          .stream_clear = on_clear,
                                          .command = on_command,
                                          .disconnected = on_disconnect};
    eaf_sendspin_client_init(&client, &config, &callbacks, NULL);
    CHECK(!eaf_sendspin_client_connect(&client, 0x7F000001u, 8927, 1000));
    CHECK(!eaf_sendspin_client_step(&client));
    CHECK(contains("GET /sendspin HTTP/1.1") && contains("Sec-WebSocket-Key:"));

    int rc = 0;
    for (unsigned i = 0; i < 200 && end_count == 0; ++i)
        rc = eaf_sendspin_client_step(&client);
    CHECK(rc == 0 || rc == EAF_IO);
    CHECK(ready_count == 1 && start_count == 1 && audio_count == 1 && end_count == 1);
    CHECK(clear_count == 1 && command_count == 1 && disconnect_count == 0);
    CHECK(audio_timestamp == 123456789 && audio_length == 16 && audio_bytes[0] == 1 &&
          audio_bytes[15] == 16);
    CHECK(decoded_contains("\"type\":\"client/hello\""));
    CHECK(decoded_contains("\"type\":\"client/state\""));
    CHECK(decoded_contains("\"type\":\"client/time\""));
    CHECK(client.handshaken);

    /* Exhausting the scripted stream closes the client and reports once. */
    eaf_sendspin_client_step(&client);
    CHECK(disconnect_count == 1 && client.state == EAF_SENDPIN_CLIENT_CLOSED);

    /* A second step on a closed client is a state error. */
    CHECK(eaf_sendspin_client_step(&client) == EAF_STATE);
    printf("sendspin client PASS (sent %zu bytes)\n", sent_size);
    return 0;
}
