#include <eaf/eaf_hal.h>
#include <eaf/eaf_lms_client.h>
#include <string.h>
int eaf_lms_buffer_limits(const eaf_format_t *fmt, const eaf_lms_buffer_request_t *request,
                          uint32_t capacity, uint32_t preferred, eaf_lms_buffer_limits_t *limits) {
    if (!fmt || !request || !limits || !eaf_format_valid(fmt) || !capacity || !preferred ||
        preferred > capacity || request->sample_bytes < 2 || request->sample_bytes > 4)
        return EAF_INVALID;
    uint32_t frame_bytes = (uint32_t)request->sample_bytes * fmt->num_channels;
    uint64_t stream = ((uint64_t)request->stream_bytes + frame_bytes - 1u) / frame_bytes;
    uint64_t output = ((uint64_t)request->output_ms * fmt->sample_rate + 999u) / 1000u;
    uint64_t ready = stream > output ? stream : output;
    if (ready < preferred)
        ready = preferred;
    *limits = (eaf_lms_buffer_limits_t){capacity, ready > capacity ? capacity : (uint32_t)ready,
                                        ready > capacity};
    return EAF_OK;
}
static void record_error(eaf_lms_client_t *c, int rc) {
    if (rc && !c->diagnostics.first_error) {
        c->diagnostics.first_error = rc;
        c->diagnostics.error_stage = c->stage;
        if (c->stage == EAF_LMS_STAGE_COMMAND || c->stage == EAF_LMS_STAGE_HTTP_CONNECT ||
            c->stage == EAF_LMS_STAGE_OUTPUT_START)
            memcpy(c->diagnostics.error_opcode, c->opcode, 4);
    }
}
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
    c->buffered_output = c->output_released = c->output_paused = false;
    c->input_eof = c->eof_sent = false;
    c->prefill_frames = 0;
    c->buffer_limits = (eaf_lms_buffer_limits_t){0};
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
    c->stage = EAF_LMS_STAGE_COMMAND;
    memcpy(c->opcode, p, 4);
    if (memcmp(p, "audg", 4) == 0) {
        if (n < 22u)
            return EAF_INVALID;
        int32_t gains[2];
        for (size_t ch = 0; ch < 2; ++ch) {
            /* 4-byte opcode, two legacy u32 gains, adjust + preamp bytes,
               then the two u32 gains. Any trailing sequence fields are ignored. */
            const uint8_t *v = p + 14u + ch * 4u;
            uint32_t raw = (uint32_t)v[0] << 24 | (uint32_t)v[1] << 16 | (uint32_t)v[2] << 8 | v[3];
            /* LMS uses unsigned 16.16. No amplification in the master stage. */
            gains[ch] = !p[12] || raw >= 65536u ? INT32_MAX : (int32_t)(raw << 15);
        }
        return c->callbacks.volume ? c->callbacks.volume(c->callbacks.ctx, gains[0], gains[1])
                                   : EAF_OK;
    }
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
        c->output_paused = false;
        return status(c, "STMr", 0);
    }
    if (s.command == 'p') {
        if (s.replay_gain || !c->callbacks.pause)
            return EAF_UNSUPPORTED;
        if (!c->streaming)
            return EAF_STATE;
        rc = c->callbacks.pause(c->callbacks.ctx, true);
        if (!rc)
            c->output_paused = true;
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
    c->stage = EAF_LMS_STAGE_HTTP_CONNECT;
    rc = hal_tcp_connect(&c->http, s.server_ipv4 ? s.server_ipv4 : c->server, s.server_port, 500);
    if (rc)
        return rc;
    c->stage = EAF_LMS_STAGE_OUTPUT_START;
    c->buffered_output = c->callbacks.start_buffered != NULL;
    if (c->buffered_output) {
        const eaf_lms_buffer_request_t request = {(uint32_t)s.threshold_kib * 1024u,
                                                  (uint32_t)s.output_threshold_ds * 100u,
                                                  (uint8_t)c->width};
        rc = c->callbacks.start_buffered(c->callbacks.ctx, &c->format, &request, &c->buffer_limits);
        if (!rc && (!c->buffer_limits.ready_frames ||
                    c->buffer_limits.ready_frames > c->buffer_limits.capacity_frames)) {
            c->callbacks.stop(c->callbacks.ctx);
            rc = EAF_INVALID;
        }
    } else {
        rc = c->callbacks.start(c->callbacks.ctx, &c->format);
    }
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
    if (!c || !cb || (!cb->start && !cb->start_buffered) || !cb->pcm || !cb->stop || !cb->eof ||
        ((cb->start_buffered != NULL) != (cb->release != NULL)))
        return EAF_INVALID;
    *c = (eaf_lms_client_t){.callbacks = *cb};
    eaf_lms_parser_init(&c->parser, command, c);
    return EAF_OK;
}
int eaf_lms_client_connect(eaf_lms_client_t *c, uint32_t server, uint16_t port,
                           const uint8_t mac[6]) {
    if (!c || !mac || (!c->callbacks.start && !c->callbacks.start_buffered) || c->control.open)
        return EAF_INVALID;
    c->diagnostics = (eaf_lms_diagnostics_t){0};
    c->stage = EAF_LMS_STAGE_CONNECT;
    int rc = hal_tcp_connect(&c->control, server, port, 500);
    if (rc) {
        record_error(c, rc);
        return rc;
    }
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
    c->step_progress = true;
    if (c->has_length && !c->remaining)
        c->body_done = true;
    return enqueue(c, "RESP", c->header, c->header_used);
}
/* Copy received body bytes into the fixed ingress ring. Content-length is
   accounted when bytes arrive, independent of when they are later decoded. */
static int ingress_fill(eaf_lms_client_t *c, const uint8_t *p, size_t n) {
    if (!n)
        return EAF_OK;
    if (n > sizeof(c->ingress) - c->ingress_used)
        return EAF_INVALID;
    if (c->has_length) {
        if (n > c->remaining)
            return EAF_INVALID;
        c->remaining -= n;
        c->body_done = !c->remaining;
    }
    c->bytes_received += n;
    memcpy(c->ingress + c->ingress_used, p, n);
    c->ingress_used += n;
    return EAF_OK;
}
/* Decode buffered body bytes into the bounded PCM chunk. Stops early when the
   chunk fills; the unwritten tail stays in the ring for the next step. */
static int ingress_decode(eaf_lms_client_t *c) {
    size_t frame_bytes = (size_t)c->width * c->format.num_channels;
    size_t consumed = 0;
    while (c->pcm_count < EAF_LMS_PCM_FRAMES && consumed < c->ingress_used) {
        c->tail[c->tail_used++] = c->ingress[consumed++];
        if (c->tail_used != frame_bytes)
            continue;
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
    if (consumed) {
        memmove(c->ingress, c->ingress + consumed, c->ingress_used - consumed);
        c->ingress_used -= consumed;
    }
    return EAF_OK;
}
static int flush_pcm(eaf_lms_client_t *c) {
    if (c->pcm_sent >= c->pcm_count)
        return EAF_OK;
    uint32_t left = c->pcm_count - c->pcm_sent;
    c->stage = EAF_LMS_STAGE_PCM;
    uint32_t wrote = c->callbacks.pcm(c->callbacks.ctx,
                                      c->pcm + (size_t)c->pcm_sent * c->format.num_channels, left);
    if (wrote > left)
        return EAF_INVALID;
    c->pcm_sent += wrote;
    c->diagnostics.pcm_frames += wrote;
    if (c->buffered_output && !c->output_released) {
        if (wrote > c->buffer_limits.capacity_frames - c->prefill_frames)
            return EAF_INVALID;
        c->prefill_frames += wrote;
    }
    c->step_progress |= wrote != 0;
    if (wrote < left) {
        ++c->diagnostics.backpressure;
        c->step_backpressure = true;
    }
    return EAF_OK;
}
/* Ready means actual accepted PCM reserve (or fully delivered short EOF).
   Startup PCM is writable only because the buffer-aware callback holds output. */
static int service_buffered(eaf_lms_client_t *c) {
    if (!c->buffered_output || c->wait_cont ||
        (c->prefill_frames < c->buffer_limits.ready_frames && !c->input_eof))
        return EAF_OK;
    if (c->wait_start && !c->ready_sent) {
        int rc = status(c, "STMl", 0);
        if (rc)
            return rc;
        c->ready_sent = true;
        c->step_progress = true;
    }
    if (!c->wait_start && !c->output_paused && !c->output_released) {
        c->stage = EAF_LMS_STAGE_OUTPUT_START;
        int rc = c->callbacks.release(c->callbacks.ctx);
        if (rc)
            return rc;
        c->output_released = true;
        c->step_progress = true;
    }
    if (c->input_eof && c->output_released && !c->eof_sent) {
        int rc = status(c, "STMd", 0);
        if (rc)
            return rc;
        c->eof_sent = true;
        c->step_progress = true;
    }
    return EAF_OK;
}
static int pump_http(eaf_lms_client_t *c) {
    if (!c->streaming)
        return EAF_OK;
    int service_rc = service_buffered(c);
    if (service_rc || !c->http.open)
        return service_rc;
    /* Headers must reach the server before cont. Legacy callbacks retain one
       private chunk until start; buffer-aware callbacks can prefill held output. */
    if (c->headers_done && c->wait_cont)
        return EAF_OK;
    if (!c->buffered_output && c->headers_done && c->wait_start &&
        (c->pcm_count || c->ingress_used || c->body_done)) {
        if (!c->ready_sent) {
            int rc = status(c, "STMl", 0);
            if (rc)
                return rc;
            c->ready_sent = true;
            c->step_progress = true;
        }
        return EAF_OK;
    }
    /* Drain decoded PCM to the output callback, then refill from the ring. */
    int rc = flush_pcm(c);
    if (rc)
        return rc;
    if (c->pcm_sent == c->pcm_count) {
        c->pcm_count = 0;
        c->pcm_sent = 0;
        rc = ingress_decode(c);
        if (rc)
            return rc;
        if (c->pcm_count) {
            c->step_progress = true;
            rc = flush_pcm(c);
            if (rc)
                return rc;
        }
    }
    /* Source complete once the body, ring and chunk are all consumed. */
    if (c->body_done && !c->ingress_used && c->pcm_sent == c->pcm_count) {
        if (c->tail_used)
            return EAF_INVALID;
        c->step_progress = true;
        c->callbacks.eof(c->callbacks.ctx);
        c->input_eof = true;
        hal_tcp_close(&c->http);
        return c->buffered_output ? service_buffered(c) : status(c, "STMd", 0);
    }
    /* Keep draining the socket into the ring even while the output callback is
       backpressured; the ring, not the output reservoir, caps socket reads. */
    if (c->body_done || c->ingress_used >= sizeof(c->ingress))
        return EAF_OK;
    size_t n = 0;
    if (c->request_sent < c->request_used) {
        c->stage = EAF_LMS_STAGE_HTTP_SEND;
        rc = hal_tcp_send(&c->http, c->request + c->request_sent, c->request_used - c->request_sent,
                          &n);
        if (rc == EAF_AGAIN)
            return EAF_OK;
        if (rc)
            return rc;
        c->request_sent += n;
        c->step_progress |= n != 0;
        return EAF_OK;
    }
    size_t space = sizeof(c->ingress) - c->ingress_used;
    size_t want = space < sizeof(c->rx) ? space : sizeof(c->rx);
    c->stage = EAF_LMS_STAGE_HTTP_RECV;
    rc = hal_tcp_recv(&c->http, c->rx, want, &n);
    if (rc == EAF_AGAIN) {
        ++c->diagnostics.recv_again;
        return EAF_OK;
    }
    if (rc == EAF_EOF) {
        if (!c->headers_done || (c->has_length && c->remaining))
            return EAF_IO;
        c->body_done = true;
        c->step_progress = true;
        return EAF_OK;
    }
    if (rc)
        return rc;
    c->diagnostics.http_bytes += n;
    c->step_progress |= n != 0;
    c->stage = EAF_LMS_STAGE_HEADERS;
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
    c->stage = EAF_LMS_STAGE_DECODE;
    return ingress_fill(c, c->rx + i, n - i);
}
int eaf_lms_client_step(eaf_lms_client_t *c) {
    if (!c)
        return EAF_STATE;
    c->step_progress = false;
    c->step_backpressure = false;
    if (!c->control.open)
        return EAF_STATE;
    size_t n = 0;
    int rc = 0;
    if (c->tx_sent < c->tx_used) {
        c->stage = EAF_LMS_STAGE_CONTROL_SEND;
        rc = hal_tcp_send(&c->control, c->tx + c->tx_sent, c->tx_used - c->tx_sent, &n);
        if (rc == EAF_AGAIN)
            rc = 0;
        if (!rc) {
            c->tx_sent += n;
            c->step_progress |= n != 0;
        }
    }
    uint8_t incoming[256];
    if (!rc) {
        c->stage = EAF_LMS_STAGE_CONTROL_RECV;
        rc = hal_tcp_recv(&c->control, incoming, sizeof(incoming), &n);
        if (rc == EAF_AGAIN)
            rc = 0;
        else if (!rc) {
            c->step_progress |= n != 0;
            rc = eaf_lms_feed(&c->parser, incoming, n);
        }
    }
    if (!rc)
        rc = pump_http(c);
    if (rc) {
        record_error(c, rc);
        eaf_lms_client_close(c);
    }
    return rc;
}
int eaf_lms_client_pump(eaf_lms_client_t *c, uint32_t max_steps, uint32_t max_us,
                        eaf_lms_pump_result_t *result) {
    if (!c || !result || !max_steps || max_steps > 64 || !max_us || max_us > 10000)
        return EAF_INVALID;
    *result = (eaf_lms_pump_result_t){0};
    uint64_t begin = hal_monotonic_time_us();
    for (;;) {
        int rc = eaf_lms_client_step(c);
        ++result->steps;
        result->progressed |= c->step_progress;
        result->backpressured |= c->step_backpressure;
        if (rc || !c->step_progress)
            return rc;
        if (result->steps >= max_steps || hal_monotonic_time_us() - begin >= max_us) {
            result->budget_exhausted = true;
            ++c->diagnostics.budget_yields;
            return EAF_OK;
        }
    }
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
    c->stage = EAF_LMS_STAGE_STATUS;
    int rc = status(c, first ? "STMs" : "STMt", 0);
    record_error(c, rc);
    if (!rc && first)
        c->started_sent = true;
    return rc;
}
