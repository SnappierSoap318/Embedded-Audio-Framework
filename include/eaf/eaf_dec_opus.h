#pragma once
#include <eaf/eaf_decoder.h>

/* Opus adapter built on libopus (third_party/opus, fixed-point build).
 *
 * Opus is packet oriented: one `push` supplies exactly one raw packet and
 * `pull` emits its decoded frames, staged so the owner may drain with a buffer
 * smaller than a packet. Decoder creation happens in `open`; the caller must
 * place this state where a preallocated allocation is acceptable (for example
 * PSRAM on ESP32). */

#define EAF_DEC_OPUS_MAX_PACKET 1500u
#define EAF_DEC_OPUS_MAX_FRAMES 5760u /* 120 ms at 48 kHz */

typedef struct {
    void *dec; /* OpusDecoder* */
    eaf_format_t format;
    uint8_t channels;
    uint8_t output_channels;
    size_t packet_length;
    uint32_t decoded_frames;
    uint32_t decoded_pos;
    uint8_t packet[EAF_DEC_OPUS_MAX_PACKET];
    int16_t temp[EAF_DEC_OPUS_MAX_FRAMES * 2u];
} eaf_dec_opus_t;

/* Pass &state as eaf_decoder_t.ctx with eaf_dec_opus_ops. */
extern const eaf_decoder_ops_t eaf_dec_opus_ops;
