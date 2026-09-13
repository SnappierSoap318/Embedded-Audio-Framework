#include <eaf/eaf_hal.h>
#include <eaf/eaf_sendspin_client.h>
#include <string.h>

#define EAF_SENDPIN_TIME_BURST_MS 200u
#define EAF_SENDPIN_TIME_STEADY_MS 3000u

static int64_t big_endian_i64(const uint8_t *p) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value = value << 8 | p[i];
    return (int64_t)value;
}

static bool find_header_end(const char *buffer, size_t length) {
    for (size_t i = 0; i + 3u < length; ++i) {
        if (!memcmp(buffer + i, "\r\n\r\n", 4))
            return true;
    }
    return false;
}

static size_t put_u64(char *dst, uint64_t value) {
    char digits[20];
    size_t n = 0;
    do {
        digits[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value);
    for (size_t i = 0; i < n; ++i)
        dst[i] = digits[n - 1u - i];
    return n;
}

static int format_host(uint32_t ipv4, uint16_t port, char *out, size_t capacity) {
    uint8_t octets[4] = {(uint8_t)(ipv4 >> 24), (uint8_t)(ipv4 >> 16), (uint8_t)(ipv4 >> 8),
                         (uint8_t)ipv4};
    size_t used = 0;
    for (size_t i = 0; i < 4; ++i) {
        if (i) {
            if (used + 1u >= capacity)
                return EAF_INVALID;
            out[used++] = '.';
        }
        if (used + 20u >= capacity)
            return EAF_INVALID;
        used += put_u64(out + used, octets[i]);
    }
    if (used + 7u >= capacity)
        return EAF_INVALID;
    out[used++] = ':';
    used += put_u64(out + used, port);
    out[used] = '\0';
    return EAF_OK;
}

static uint32_t time_interval_ms(const eaf_sendspin_client_t *client) {
    if (!eaf_sendspin_time_synchronized(&client->filter))
        return EAF_SENDPIN_TIME_BURST_MS;
    int64_t error = eaf_sendspin_time_error(&client->filter);
    if (error < 1000)
        return EAF_SENDPIN_TIME_STEADY_MS;
    if (error < 2000)
        return 1000u;
    if (error < 5000)
        return 500u;
    return 200u;
}

static int queue_frame(eaf_sendspin_client_t *client, uint8_t opcode, const uint8_t *payload,
                       size_t length) {
    size_t frame_length = 0;
    uint32_t mask = eaf_sendspin_ws_prng_next(&client->prng);
    if (eaf_sendspin_ws_encode(client->frame, sizeof(client->frame), opcode, payload, length, mask,
                               &frame_length))
        return EAF_INVALID;
    if (client->wire_offset) {
        size_t pending = client->wire_length - client->wire_offset;
        if (pending)
            memmove(client->wire, client->wire + client->wire_offset, pending);
        client->wire_length = pending;
        client->wire_offset = 0;
    }
    if (client->wire_length + frame_length > sizeof(client->wire))
        return EAF_INVALID;
    memcpy(client->wire + client->wire_length, client->frame, frame_length);
    client->wire_length += frame_length;
    return EAF_OK;
}

static int queue_json(eaf_sendspin_client_t *client, size_t length) {
    return queue_frame(client, EAF_SENDPIN_WS_TEXT, (const uint8_t *)client->json, length);
}

static int flush(eaf_sendspin_client_t *client) {
    while (client->wire_offset < client->wire_length) {
        size_t sent = 0;
        int rc = hal_tcp_send(&client->tcp, client->wire + client->wire_offset,
                              client->wire_length - client->wire_offset, &sent);
        client->wire_offset += sent;
        if (rc == EAF_AGAIN)
            return EAF_AGAIN;
        if (rc)
            return rc;
        if (!sent)
            return EAF_AGAIN;
    }
    client->wire_offset = 0;
    client->wire_length = 0;
    return EAF_OK;
}

static void notify_closed(eaf_sendspin_client_t *client) {
    if (client->tcp.open)
        hal_tcp_close(&client->tcp);
    client->state = EAF_SENDPIN_CLIENT_CLOSED;
    client->handshaken = false;
    client->stream_active = false;
    if (!client->notified) {
        client->notified = true;
        if (client->callbacks.disconnected)
            client->callbacks.disconnected(client->callback_ctx);
    }
}

static int fail(eaf_sendspin_client_t *client) {
    notify_closed(client);
    return EAF_IO;
}

static void send_time(eaf_sendspin_client_t *client) {
    uint64_t now = hal_monotonic_time_us();
    size_t written = 0;
    if (!eaf_sendspin_build_client_time(client->json, sizeof(client->json), (int64_t)now, &written))
        (void)queue_json(client, written);
    client->next_time_us = now + (uint64_t)time_interval_ms(client) * 1000u;
}

static int on_server_hello(eaf_sendspin_client_t *client, const char *json, size_t length) {
    eaf_sendspin_server_hello_t hello;
    if (eaf_sendspin_parse_server_hello(json, length, &hello))
        return EAF_INVALID;
    if (!hello.player_active)
        return EAF_UNSUPPORTED;
    if (client->callbacks.ready)
        client->callbacks.ready(client->callback_ctx, &hello);
    eaf_sendspin_player_state_t state = {.volume = client->config.volume,
                                         .muted = false,
                                         .static_delay_ms = client->config.static_delay_ms,
                                         .required_lead_time_ms =
                                             client->config.required_lead_time_ms,
                                         .min_buffer_ms = client->config.min_buffer_ms};
    size_t written = 0;
    if (eaf_sendspin_build_client_state(client->json, sizeof(client->json), &state, &written) ||
        queue_json(client, written))
        return EAF_INVALID;
    client->handshaken = true;
    send_time(client);
    return EAF_OK;
}

static int dispatch_text(eaf_sendspin_client_t *client, const char *json, size_t length) {
    eaf_sendspin_json_value_t type;
    if (eaf_sendspin_json_get(json, length, "type", &type) || type.type != EAF_SENDPIN_JSON_STRING)
        return EAF_OK;
    if (eaf_sendspin_json_string_equals(&type, "server/hello"))
        return on_server_hello(client, json, length);
    if (eaf_sendspin_json_string_equals(&type, "server/time")) {
        eaf_sendspin_server_time_t time_message;
        if (eaf_sendspin_parse_server_time(json, length, &time_message))
            return EAF_INVALID;
        eaf_sendspin_time_filter_exchange(&client->filter, time_message.client_transmitted,
                                          time_message.server_received,
                                          time_message.server_transmitted, hal_monotonic_time_us());
        return EAF_OK;
    }
    if (eaf_sendspin_json_string_equals(&type, "stream/start")) {
        if (eaf_sendspin_parse_stream_start(json, length, &client->stream))
            return EAF_INVALID;
        if (client->stream.codec != EAF_SENDPIN_CODEC_PCM)
            return EAF_UNSUPPORTED;
        client->stream_active = true;
        if (client->callbacks.stream_start)
            client->callbacks.stream_start(client->callback_ctx, &client->stream);
        return EAF_OK;
    }
    if (eaf_sendspin_json_string_equals(&type, "stream/end")) {
        eaf_sendspin_stream_end_t end;
        if (eaf_sendspin_parse_stream_end(json, length, &end))
            return EAF_INVALID;
        client->stream_active = false;
        if (client->callbacks.stream_end)
            client->callbacks.stream_end(client->callback_ctx, end.end_player);
        return EAF_OK;
    }
    if (eaf_sendspin_json_string_equals(&type, "group/update")) {
        eaf_sendspin_group_update_t group;
        if (eaf_sendspin_parse_group_update(json, length, &group))
            return EAF_INVALID;
        if (client->callbacks.group_update)
            client->callbacks.group_update(client->callback_ctx, &group);
        return EAF_OK;
    }
    if (eaf_sendspin_json_string_equals(&type, "server/command")) {
        eaf_sendspin_server_command_t command;
        if (eaf_sendspin_parse_server_command(json, length, &command))
            return EAF_INVALID;
        if (command.command == EAF_SENDPIN_COMMAND_SET_STATIC_DELAY && command.static_delay_ms >= 0)
            client->config.static_delay_ms = command.static_delay_ms;
        if (client->callbacks.command)
            client->callbacks.command(client->callback_ctx, &command);
        return EAF_OK;
    }
    return EAF_OK;
}

static int on_message(void *ctx, uint8_t opcode, const uint8_t *payload, size_t length) {
    eaf_sendspin_client_t *client = ctx;
    if (opcode == EAF_SENDPIN_WS_PING)
        return queue_frame(client, EAF_SENDPIN_WS_PONG, payload, length) ? EAF_INVALID : EAF_OK;
    if (opcode == EAF_SENDPIN_WS_PONG)
        return EAF_OK;
    if (opcode == EAF_SENDPIN_WS_CLOSE) {
        notify_closed(client);
        return EAF_OK;
    }
    if (opcode == EAF_SENDPIN_WS_TEXT)
        return dispatch_text(client, (const char *)payload, length);
    if (opcode == EAF_SENDPIN_WS_BINARY) {
        if (length < 9u || payload[0] != 0x04u)
            return EAF_OK;
        int64_t timestamp = big_endian_i64(payload + 1);
        if (client->callbacks.audio)
            client->callbacks.audio(client->callback_ctx, &client->stream, timestamp, payload + 9u,
                                    length - 9u);
    }
    return EAF_OK;
}

void eaf_sendspin_client_init(eaf_sendspin_client_t *client, const eaf_sendspin_config_t *config,
                              const eaf_sendspin_callbacks_t *callbacks, void *ctx) {
    *client = (eaf_sendspin_client_t){0};
    client->config = *config;
    if (callbacks)
        client->callbacks = *callbacks;
    client->callback_ctx = ctx;
    eaf_sendspin_time_filter_init(&client->filter);
    eaf_sendspin_ws_rx_init(&client->rx, client->message, sizeof(client->message));
}

int eaf_sendspin_client_connect(eaf_sendspin_client_t *client, uint32_t ipv4, uint16_t port,
                                uint32_t timeout_ms) {
    if (!client || client->state != EAF_SENDPIN_CLIENT_CLOSED || client->tcp.open)
        return EAF_INVALID;
    eaf_sendspin_ws_prng_init(&client->prng, (uint32_t)hal_monotonic_time_us());
    char key[25];
    eaf_sendspin_ws_client_key(&client->prng, key);
    char host[32];
    if (format_host(ipv4, port, host, sizeof(host)))
        return EAF_INVALID;
    int rc = hal_tcp_connect(&client->tcp, ipv4, port, timeout_ms);
    if (rc)
        return rc;
    size_t written = 0;
    if (eaf_sendspin_ws_build_upgrade(client->json, sizeof(client->json), host, "/sendspin", key,
                                      &written) ||
        written > sizeof(client->wire))
        return fail(client);
    memcpy(client->wire, client->json, written);
    client->wire_length = written;
    client->wire_offset = 0;
    client->http_used = 0;
    client->notified = false;
    client->state = EAF_SENDPIN_CLIENT_HTTP;
    return EAF_OK;
}

int eaf_sendspin_client_step(eaf_sendspin_client_t *client) {
    if (!client || client->state == EAF_SENDPIN_CLIENT_CLOSED)
        return EAF_STATE;
    int rc = flush(client);
    if (rc == EAF_AGAIN)
        return EAF_OK;
    if (rc)
        return fail(client);

    if (client->state == EAF_SENDPIN_CLIENT_HTTP) {
        size_t received = 0;
        rc = hal_tcp_recv(&client->tcp, client->http + client->http_used,
                          sizeof(client->http) - client->http_used, &received);
        if (rc == EAF_AGAIN)
            return EAF_OK;
        if (rc)
            return fail(client);
        client->http_used += received;
        if (!find_header_end(client->http, client->http_used)) {
            if (client->http_used == sizeof(client->http))
                return fail(client);
            return EAF_OK;
        }
        if (eaf_sendspin_ws_check_upgrade_response(client->http, client->http_used))
            return fail(client);
        eaf_sendspin_client_hello_t hello = {.client_id = client->config.client_id,
                                             .name = client->config.name,
                                             .product_name = client->config.product_name,
                                             .manufacturer = client->config.manufacturer,
                                             .software_version = client->config.software_version,
                                             .sample_rate = client->config.sample_rate,
                                             .channels = client->config.channels,
                                             .bit_depth = client->config.bit_depth,
                                             .buffer_capacity = client->config.buffer_capacity,
                                             .support_volume = client->config.support_volume,
                                             .support_mute = client->config.support_mute};
        size_t written = 0;
        if (eaf_sendspin_build_client_hello(client->json, sizeof(client->json), &hello, &written) ||
            queue_json(client, written))
            return fail(client);
        client->state = EAF_SENDPIN_CLIENT_WS;
        return EAF_OK;
    }

    uint64_t now = hal_monotonic_time_us();
    if (client->handshaken && now >= client->next_time_us)
        send_time(client);
    size_t received = 0;
    rc = hal_tcp_recv(&client->tcp, client->rx_bytes, sizeof(client->rx_bytes), &received);
    if (rc == EAF_AGAIN)
        return EAF_OK;
    if (rc)
        return fail(client);
    if (eaf_sendspin_ws_rx_feed(&client->rx, client->rx_bytes, received, on_message, client))
        return fail(client);
    return EAF_OK;
}

void eaf_sendspin_client_close(eaf_sendspin_client_t *client) {
    notify_closed(client);
}

bool eaf_sendspin_client_ready(const eaf_sendspin_client_t *client) {
    return client && client->handshaken;
}

bool eaf_sendspin_client_time_synchronized(const eaf_sendspin_client_t *client) {
    return client && eaf_sendspin_time_synchronized(&client->filter);
}

int64_t eaf_sendspin_client_compute_client_time(const eaf_sendspin_client_t *client,
                                                int64_t server_us) {
    return client ? eaf_sendspin_compute_client_time(&client->filter, server_us) : server_us;
}

int64_t eaf_sendspin_client_compute_server_time(const eaf_sendspin_client_t *client,
                                                int64_t client_us) {
    return client ? eaf_sendspin_compute_server_time(&client->filter, client_us) : client_us;
}
