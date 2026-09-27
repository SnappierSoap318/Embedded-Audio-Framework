#pragma once
#include <eaf/eaf_dec_input.h>
#include <eaf/eaf_decoder.h>

/* Built-in PCM passthrough adapter. It buffers little-endian PCM bytes and
   converts whole frames to interleaved Q1.31 on pull, so partial input frames
   may be split arbitrarily across pushes. Mono input upmixes to the requested
   output channel count; other channel changes are rejected. */

#define EAF_DECODER_PCM_RING_BYTES 4096u

typedef struct {
    eaf_format_t format;
    uint8_t input_channels;
    uint8_t input_bits;
    uint8_t output_channels;
    size_t sample_bytes;
    size_t frame_bytes;
    eaf_dec_input_t input;
    uint8_t ring[EAF_DECODER_PCM_RING_BYTES];
} eaf_decoder_pcm_t;

/* Pass &state as eaf_decoder_t.ctx with eaf_decoder_pcm_ops. */
extern const eaf_decoder_ops_t eaf_decoder_pcm_ops;
