#pragma once
#include <eaf/eaf_dec_input.h>
#include <eaf/eaf_decoder.h>

/* MP3 adapter built on dr_mp3 (third_party/dr_libs).
 *
 * dr_mp3 0.7.4 exposes no 32-bit read API, so frames decode to int16 and are
 * widened to Q1.31. Like FLAC it streams through a read callback that must fill
 * its buffer in one call, so it shares the bracketed input ring and optional
 * await hook. The decoder object is allocated at `open` (off the audio path). */

#define EAF_DEC_MP3_RING_BYTES 16384u
#define EAF_DEC_MP3_TEMP_FRAMES 1152u

typedef struct {
    void *mp3; /* drmp3* */
    eaf_dec_input_t input;
    uint8_t ring[EAF_DEC_MP3_RING_BYTES];
    eaf_format_t format;
    uint8_t cfg_channels; /* 0 when the container did not declare a count */
    uint8_t mp3_channels;
    uint8_t output_channels;
    bool initialized;
    int16_t temp[EAF_DEC_MP3_TEMP_FRAMES * 2u];
} eaf_dec_mp3_t;

void eaf_dec_mp3_configure(eaf_dec_mp3_t *state, eaf_dec_await_fn await, void *await_ctx);
void eaf_dec_mp3_finish(eaf_dec_mp3_t *state);

/* Pass &state as eaf_decoder_t.ctx with eaf_dec_mp3_ops. */
extern const eaf_decoder_ops_t eaf_dec_mp3_ops;
