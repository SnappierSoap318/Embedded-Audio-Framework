#pragma once
#include <eaf/eaf_types.h>
#define EAF_LMS_MAX_PACKET 2048u
typedef struct {
    uint8_t command, autostart, codec, sample_size, sample_rate, channels, endianness;
    uint8_t flags;
    uint32_t replay_gain, server_ipv4;
    uint16_t server_port;
    const uint8_t *request;
    size_t request_length;
} eaf_lms_stream_t;
/* Borrowed packet/request memory is valid only until the callback returns.
   Transport/control thread owns the parser. Callback must copy queued data. */
typedef int (*eaf_lms_packet_fn)(void *, const uint8_t *packet, size_t length);
typedef struct {
    uint8_t packet[EAF_LMS_MAX_PACKET], prefix[2];
    size_t prefix_used, used, expected;
    bool failed;
    eaf_lms_packet_fn on_packet;
    void *ctx;
} eaf_lms_parser_t;
void eaf_lms_parser_init(eaf_lms_parser_t *p, eaf_lms_packet_fn callback, void *ctx);
/* TCP fragments and coalesced messages accepted; fatal error requires reset/reconnect. */
int eaf_lms_feed(eaf_lms_parser_t *p, const uint8_t *bytes, size_t length);
int eaf_lms_parse_stream(const uint8_t *packet, size_t length, eaf_lms_stream_t *out);
/* HELO uses outgoing 4-byte opcode + 32-bit BE payload length (not incoming framing).
   Capabilities are supplied explicitly; do not advertise unimplemented codecs/sync. */
int eaf_lms_helo(uint8_t *dst, size_t capacity, const uint8_t mac[6], const char *capabilities,
                 size_t *written);
