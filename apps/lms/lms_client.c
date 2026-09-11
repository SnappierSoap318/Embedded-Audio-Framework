#include <eaf/eaf_lms_client.h>
#include <string.h>
static void put32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(n >> ((3u - i) * 8u));
}
static int enqueue(eaf_lms_client_t *c, const char opcode[4], const void *body, size_t n) {
    if (c->tx_sent) {
        memmove(c->tx, c->tx + c->tx_sent, c->tx_used - c->tx_sent);
        c->tx_used -= c->tx_sent;
        c->tx_sent = 0;
    }
    if (n > sizeof(c->tx) - 8u || c->tx_used > sizeof(c->tx) - 8u - n)
        return EAF_IO;
    uint8_t *p = c->tx + c->tx_used;
    memcpy(p, opcode, 4);
    put32(p + 4, (uint32_t)n);
    if (n)
        memcpy(p + 8, body, n);
    c->tx_used += 8u + n;
    return EAF_OK;
}
static int status(eaf_lms_client_t *c, const char event[4], uint32_t timestamp) {
    uint8_t body[53] = {0};
    memcpy(body, event, 4);
    put32(body + 15, (uint32_t)(c->bytes_received >> 32));
    put32(body + 19, (uint32_t)c->bytes_received);
    put32(body + 29, c->playback.buffer_bytes);
    put32(body + 33, c->playback.queued_bytes);
    put32(body + 37, c->playback.elapsed_ms / 1000u);
    put32(body + 43, c->playback.elapsed_ms);
    put32(body + 47, timestamp);
    return enqueue(c, "STAT", body, sizeof(body));
}
static void stop_stream(eaf_lms_client_t *c) {
    hal_tcp_close(&c->http);
    if (c->streaming)
        c->callbacks.stop(c->callbacks.ctx);
    c->streaming = false;
    c->pcm_count = 0;
    c->pcm_sent = 0;
    c->body_done = false;
    c->wait_cont = false;
    c->wait_start = false;
    c->ready_sent = false;
    c->started_sent = false;
    c->playback = (eaf_lms_playback_t){0};
    c->bytes_received = 0;
}
static int command(void *ctx, const uint8_t *p, size_t n) {
    eaf_lms_client_t *c = ctx;
    if (!memcmp(p, "cont", 4)) {
        if (n < 9u)
            return EAF_INVALID;
        if (!c->streaming || !c->wait_cont)
            return EAF_OK;
        /* Metadata insertion and looping require separate body handling. */
        if (p[4] || p[5] || p[6] || p[7] || p[8])
            return EAF_UNSUPPORTED;
        c->wait_cont = false;
        return EAF_OK;
    }
    if (memcmp(p, "strm", 4) != 0)
        return EAF_OK; /* unsupported control opcodes ignored */
    eaf_lms_stream_t s;
    int rc = eaf_lms_parse_stream(p, n, &s);
    if (rc)
        return rc;
    if (s.command == 't')
        return status(c, "STMt", s.replay_gain);
    if (s.command == 'q' || s.command == 'f') {
        stop_stream(c);
        return status(c, "STMf", 0);
    }
    if (s.command == 'u') {
        /* Initial release or immediate output resume; timestamps need scheduling. */
        if (s.replay_gain)
            return EAF_UNSUPPORTED;
        if (!c->streaming || c->wait_cont)
            return EAF_STATE;
        if (c->wait_start) {
            if (!c->ready_sent)
                return EAF_STATE;
            c->wait_start = false;
        } else {
            if (!c->callbacks.pause)
                return EAF_UNSUPPORTED;
            rc = c->callbacks.pause(c->callbacks.ctx, false);
            if (rc)
                return rc;
        }
        return status(c, "STMr", 0);
    }
    if (s.command == 'p') {
        if (s.replay_gain || !c->callbacks.pause)
            return EAF_UNSUPPORTED;
        if (!c->streaming)
            return EAF_STATE;
        rc = c->callbacks.pause(c->callbacks.ctx, true);
        return rc ? rc : status(c, "STMp", 0);
    }
    if (s.command != 's')
        return EAF_UNSUPPORTED;
    stop_stream(c);
    static const uint32_t rates[] = {11025, 22050, 32000, 44100, 48000, 8000,
                                     12000, 16000, 24000, 96000, 88200};
    if ((s.flags & 0x20u) || s.codec != 'p' || s.autostart < '0' || s.autostart > '3' ||
        s.sample_size < '1' || s.sample_size > '3' || s.sample_rate < '0' ||
        (unsigned)(s.sample_rate - '0') >= sizeof(rates) / sizeof(rates[0]) ||
        (s.channels != '1' && s.channels != '2') || (s.endianness != '0' && s.endianness != '1') ||
        s.request_length < 4u || s.request_length > sizeof(c->request) ||
        memcmp(s.request + s.request_length - 4u, "\r\n\r\n", 4) != 0)
        return EAF_UNSUPPORTED;
    c->width = (unsigned)(s.sample_size - '0') + 1u;
    c->big_endian = s.endianness == '0';
    c->format = (eaf_format_t){rates[s.sample_rate - '0'], (uint8_t)(s.channels - '0'),
                               s.channels == '1' ? 8u : 3u};
    rc = hal_tcp_connect(&c->http, s.server_ipv4 ? s.server_ipv4 : c->server, s.server_port, 500);
    if (rc)
        return rc;
    rc = c->callbacks.start(c->callbacks.ctx, &c->format);
    if (rc) {
        hal_tcp_close(&c->http);
        return rc;
    }
    c->streaming = true;
    c->header_used = 0;
    c->headers_done = false;
    c->has_length = false;
    c->wait_cont = s.autostart >= '2';
    c->wait_start = s.autostart == '0' || s.autostart == '2';
    c->tail_used = 0;
    c->bytes_received = 0;
    c->request_used = s.request_length;
    c->request_sent = 0;
    memcpy(c->request, s.request, s.request_length);
    return status(c, "STMc", 0);
}
int eaf_lms_client_init(eaf_lms_client_t *c, const eaf_lms_callbacks_t *cb) {
    if (!c || !cb || !cb->start || !cb->pcm || !cb->stop || !cb->eof)
        return EAF_INVALID;
    *c = (eaf_lms_client_t){.callbacks = *cb};
    eaf_lms_parser_init(&c->parser, command, c);
    return EAF_OK;
}
int eaf_lms_client_connect(eaf_lms_client_t *c, uint32_t server, uint16_t port,
                           const uint8_t mac[6]) {
    if (!c || !mac || !c->callbacks.start || c->control.open)
        return EAF_INVALID;
    int rc = hal_tcp_connect(&c->control, server, port, 500);
    if (rc)
        return rc;
    c->server = server;
    c->tx_sent = 0;
    eaf_lms_parser_init(&c->parser, command, c);
    rc = eaf_lms_helo(c->tx, sizeof(c->tx), mac, "Model=eaf,MaxSampleRate=96000,pcm", &c->tx_used);
    if (rc)
        hal_tcp_close(&c->control);
    return rc;
}
static bool key(const char *p, size_t n, const char *name) {
    size_t len = strlen(name);
    if (n < len)
        return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)p[i];
        if (ch >= 'A' && ch <= 'Z')
            ch = (unsigned char)(ch + ('a' - 'A'));
        if (ch != (unsigned char)name[i])
            return false;
    }
    return true;
}
static int headers(eaf_lms_client_t *c) {
    const char *p = c->header;
    if (c->header_used < 12 ||
        (memcmp(p, "HTTP/1.0 200 ", 13) != 0 && memcmp(p, "HTTP/1.1 200 ", 13) != 0))
        return EAF_IO;
    p = strstr(p, "\r\n");
    if (!p)
        return EAF_INVALID;
    p += 2;
    while (*p && memcmp(p, "\r\n", 2) != 0) {
        const char *end = strstr(p, "\r\n");
        if (!end)
            return EAF_INVALID;
        size_t len = (size_t)(end - p);
        if (key(p, len, "transfer-encoding:") || key(p, len, "content-encoding:") ||
            key(p, len, "icy-metaint:"))
            return EAF_UNSUPPORTED;
        if (key(p, len, "content-length:")) {
            if (c->has_length)
                return EAF_INVALID;
            const char *v = p + 15;
            while (v < end && (*v == ' ' || *v == '\t'))
                ++v;
            uint64_t value = 0;
            bool digit = false;
            while (v < end && *v >= '0' && *v <= '9') {
                unsigned d = (unsigned)(*v++ - '0');
                if (value > (UINT64_MAX - d) / 10u)
                    return EAF_INVALID;
                value = value * 10u + d;
                digit = true;
            }
            while (v < end && (*v == ' ' || *v == '\t'))
                ++v;
            if (!digit || v != end)
                return EAF_INVALID;
            c->remaining = value;
            c->has_length = true;
        }
        p = end + 2;
    }
    c->headers_done = true;
    if (c->has_length && !c->remaining)
        c->body_done = true;
    return enqueue(c, "RESP", c->header, c->header_used);
}
static int decode(eaf_lms_client_t *c, const uint8_t *p, size_t n) {
    size_t frame_bytes = (size_t)c->width * c->format.num_channels;
    if (c->has_length && n > c->remaining)
        return EAF_INVALID;
    c->bytes_received += n;
    if (c->has_length) {
        c->remaining -= n;
        c->body_done = !c->remaining;
    }
    for (size_t i = 0; i < n; ++i) {
        c->tail[c->tail_used++] = p[i];
        if (c->tail_used != frame_bytes)
            continue;
        if (c->pcm_count >= EAF_LMS_PCM_FRAMES)
            return EAF_IO;
        for (size_t ch = 0; ch < c->format.num_channels; ++ch) {
            uint32_t raw = 0;
            for (size_t b = 0; b < c->width; ++b) {
                size_t byte = c->big_endian ? b : c->width - b - 1u;
                raw = (raw << 8) | c->tail[ch * c->width + byte];
            }
            unsigned bits = c->width * 8u;
            int64_t sample = raw;
            if (raw & (UINT32_C(1) << (bits - 1u)))
                sample -= INT64_C(1) << bits;
            c->pcm[(size_t)c->pcm_count * c->format.num_channels + ch] =
                (int32_t)(sample * (INT64_C(1) << (32u - bits)));
        }
        ++c->pcm_count;
        c->tail_used = 0;
    }
    return EAF_OK;
}
static int pump_http(eaf_lms_client_t *c) {
    if (!c->streaming || !c->http.open)
        return EAF_OK;
    /* Headers must reach the server before cont. Keep at most one decoded chunk
       privately until both gates open; never publish PCM while waiting. */
    if (c->headers_done && c->wait_cont)
        return EAF_OK;
    if (c->headers_done && c->wait_start && (c->pcm_count || c->body_done)) {
        if (!c->ready_sent) {
            int rc = status(c, "STMl", 0);
            if (rc)
                return rc;
            c->ready_sent = true;
        }
        return EAF_OK;
    }
    if (c->pcm_sent < c->pcm_count) {
        uint32_t left = c->pcm_count - c->pcm_sent;
        uint32_t wrote = c->callbacks.pcm(
            c->callbacks.ctx, c->pcm + (size_t)c->pcm_sent * c->format.num_channels, left);
        if (wrote > left)
            return EAF_INVALID;
        c->pcm_sent += wrote;
        if (c->pcm_sent < c->pcm_count)
            return EAF_OK;
    }
    c->pcm_count = 0;
    c->pcm_sent = 0;
    if (c->body_done) {
        if (c->tail_used)
            return EAF_INVALID;
        c->callbacks.eof(c->callbacks.ctx);
        hal_tcp_close(&c->http);
        return status(c, "STMd", 0);
    }
    size_t n = 0;
    if (c->request_sent < c->request_used) {
        int rc = hal_tcp_send(&c->http, c->request + c->request_sent,
                              c->request_used - c->request_sent, &n);
        if (rc == EAF_AGAIN)
            return EAF_OK;
        if (rc)
            return rc;
        c->request_sent += n;
        return EAF_OK;
    }
    int rc = hal_tcp_recv(&c->http, c->rx, sizeof(c->rx), &n);
    if (rc == EAF_AGAIN)
        return EAF_OK;
    if (rc == EAF_EOF) {
        if (!c->headers_done || (c->has_length && c->remaining) || c->tail_used)
            return EAF_IO;
        c->body_done = true;
        return EAF_OK;
    }
    if (rc)
        return rc;
    size_t i = 0;
    while (i < n && !c->headers_done) {
        if (c->header_used == EAF_LMS_MAX_PACKET)
            return EAF_INVALID;
        char ch = (char)c->rx[i++];
        if (!ch)
            return EAF_INVALID;
        c->header[c->header_used++] = ch;
        c->header[c->header_used] = '\0';
        if (c->header_used >= 4 && !memcmp(c->header + c->header_used - 4, "\r\n\r\n", 4)) {
            rc = headers(c);
            if (rc)
                return rc;
        }
    }
    return decode(c, c->rx + i, n - i);
}
int eaf_lms_client_step(eaf_lms_client_t *c) {
    if (!c || !c->control.open)
        return EAF_STATE;
    size_t n = 0;
    int rc = 0;
    if (c->tx_sent < c->tx_used) {
        rc = hal_tcp_send(&c->control, c->tx + c->tx_sent, c->tx_used - c->tx_sent, &n);
        if (rc == EAF_AGAIN)
            rc = 0;
        if (!rc)
            c->tx_sent += n;
    }
    uint8_t incoming[256];
    if (!rc) {
        rc = hal_tcp_recv(&c->control, incoming, sizeof(incoming), &n);
        if (rc == EAF_AGAIN)
            rc = 0;
        else if (!rc)
            rc = eaf_lms_feed(&c->parser, incoming, n);
    }
    if (!rc)
        rc = pump_http(c);
    if (rc)
        eaf_lms_client_close(c);
    return rc;
}
void eaf_lms_client_close(eaf_lms_client_t *c) {
    if (!c)
        return;
    stop_stream(c);
    hal_tcp_close(&c->control);
    c->tx_used = 0;
    c->tx_sent = 0;
}
int eaf_lms_client_report_playback(eaf_lms_client_t *c, const eaf_lms_playback_t *p) {
    if (!c || !p || p->queued_bytes > p->buffer_bytes)
        return EAF_INVALID;
    if (!c->control.open || !c->streaming || c->wait_cont || c->wait_start)
        return EAF_STATE;
    if (c->started_sent && !p->started)
        return EAF_STATE;
    c->playback = *p;
    bool first = p->started && !c->started_sent;
    int rc = status(c, first ? "STMs" : "STMt", 0);
    if (!rc && first)
        c->started_sent = true;
    return rc;
}
