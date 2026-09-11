#include <eaf/eaf_lms.h>
#include <string.h>
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
void eaf_lms_parser_init(eaf_lms_parser_t *p, eaf_lms_packet_fn callback, void *ctx) {
    *p = (eaf_lms_parser_t){.on_packet = callback, .ctx = ctx};
}
int eaf_lms_feed(eaf_lms_parser_t *p, const uint8_t *bytes, size_t length) {
    if (!p || !p->on_packet || (!bytes && length))
        return EAF_INVALID;
    if (p->failed)
        return EAF_STATE;
    while (length) {
        if (p->prefix_used < 2u) {
            p->prefix[p->prefix_used++] = *bytes++;
            --length;
            if (p->prefix_used < 2u)
                continue;
            p->expected = (size_t)p->prefix[0] * 256u + p->prefix[1];
            if (p->expected < 4u || p->expected > sizeof(p->packet)) {
                p->failed = true;
                return EAF_INVALID;
            }
        }
        size_t take = p->expected - p->used;
        if (take > length)
            take = length;
        memcpy(p->packet + p->used, bytes, take);
        p->used += take;
        bytes += take;
        length -= take;
        if (p->used == p->expected) {
            int rc = p->on_packet(p->ctx, p->packet, p->expected);
            p->prefix_used = 0;
            p->used = 0;
            p->expected = 0;
            if (rc) {
                p->failed = true;
                return rc;
            }
        }
    }
    return EAF_OK;
}
int eaf_lms_parse_stream(const uint8_t *p, size_t length, eaf_lms_stream_t *out) {
    if (!p || !out || length < 28u || memcmp(p, "strm", 4) != 0)
        return EAF_INVALID;
    *out = (eaf_lms_stream_t){.command = p[4],
                              .autostart = p[5],
                              .codec = p[6],
                              .sample_size = p[7],
                              .sample_rate = p[8],
                              .channels = p[9],
                              .endianness = p[10],
                              .flags = p[15],
                              .replay_gain = be32(p + 18),
                              .server_port = (uint16_t)((uint16_t)p[22] * 256u + p[23]),
                              .server_ipv4 = be32(p + 24),
                              .request = p + 28,
                              .request_length = length - 28u};
    return EAF_OK;
}
int eaf_lms_helo(uint8_t *dst, size_t capacity, const uint8_t mac[6], const char *capabilities,
                 size_t *written) {
    if (!dst || !mac || !capabilities || !written)
        return EAF_INVALID;
    *written = 0;
    size_t len = 0;
    while (len < EAF_LMS_MAX_PACKET && capabilities[len])
        ++len;
    if (len == EAF_LMS_MAX_PACKET || capacity < 44u + len)
        return EAF_INVALID;
    memset(dst, 0, 44);
    /* Wire opcode is exactly four bytes, not a C string. */
    // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
    memcpy(dst, "HELO", 4);
    uint32_t payload = (uint32_t)len + 36u;
    for (size_t i = 0; i < 4; ++i)
        dst[4u + i] = (uint8_t)(payload >> ((3u - i) * 8u));
    dst[8] = 12; /* SqueezePlay device family; capabilities identify EAF. */
    memcpy(dst + 10, mac, 6);
    memcpy(dst + 44, capabilities, len);
    *written = 44u + len;
    return EAF_OK;
}
