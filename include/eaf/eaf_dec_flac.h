#pragma once
#include <eaf/eaf_dec_input.h>
#include <eaf/eaf_decoder.h>

/* FLAC adapter built on dr_flac (third_party/dr_libs).
 *
 * dr_flac streams through a read callback that must fill its whole 4 KB L2
 * buffer in one call or it latches end-of-stream, so this adapter buffers
 * compressed bytes in a caller-owned state and blocks (through the input's
 * await hook) when the decode owner needs more input than is buffered. With no
 * await hook the read returns a short count, which is only correct when the
 * complete stream is already buffered or at end-of-stream. */

#define EAF_DEC_FLAC_RING_BYTES 16384u
#define EAF_DEC_FLAC_TEMP_FRAMES 512u

typedef struct {
    void *flac; /* drflac* */
    eaf_dec_input_t input;
    uint8_t ring[EAF_DEC_FLAC_RING_BYTES];
    eaf_format_t format;
    uint8_t flac_channels;
    uint8_t output_channels;
    int32_t temp[EAF_DEC_FLAC_TEMP_FRAMES];
} eaf_dec_flac_t;

/* Set the optional await hook before opening; pass NULL for a fully buffered
   stream. */
void eaf_dec_flac_configure(eaf_dec_flac_t *state, eaf_dec_await_fn await, void *await_ctx);
/* Mark end of compressed input so a trailing partial buffer can be decoded.
   The caller must also release any waiter blocked in `await`. */
void eaf_dec_flac_finish(eaf_dec_flac_t *state);

/* Pass &state as eaf_decoder_t.ctx with eaf_dec_flac_ops. */
extern const eaf_decoder_ops_t eaf_dec_flac_ops;
