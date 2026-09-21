#pragma once
#include <eaf/eaf_decoder.h>

/* Ogg Vorbis adapter built on stb_vorbis (third_party/stb).
 *
 * stb_vorbis's pushdata API is naturally non-blocking: `open` needs the stream
 * headers and `pull` reports when more bytes are required, so no await hook is
 * used. The pending input is kept in a linear buffer. A decoded Vorbis frame is
 * emitted whole, so `pull` must be given a buffer at least as large as the
 * stream's maximum frame (see stb_vorbis_info.max_frame_size). */

#define EAF_DEC_VORBIS_BUFFER_BYTES 32768u

typedef struct {
    void *v; /* stb_vorbis* */
    uint8_t buffer[EAF_DEC_VORBIS_BUFFER_BYTES];
    size_t length;
    eaf_format_t format;
    uint8_t vorbis_channels; /* 0 until the stream headers are parsed */
    uint8_t output_channels;
} eaf_dec_vorbis_t;

/* Pass &state as eaf_decoder_t.ctx with eaf_dec_vorbis_ops. */
extern const eaf_decoder_ops_t eaf_dec_vorbis_ops;
