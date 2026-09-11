#pragma once
#include <eaf/eaf_source.h>
#define EAF_WAV_BLOCK_FRAMES 256u
typedef struct {
    eaf_reader_t reader;
    uint64_t data_offset, position, total_frames;
    eaf_format_t format;
    uint16_t bits, block_align;
    unsigned char scratch[EAF_WAV_BLOCK_FRAMES * 2u * 4u];
} eaf_wav_t;
/* Classic little-endian PCM RIFF/WAVE, mono/stereo, 16/24/32 bit.
   Rejects float, compressed, extensible, RIFX and RF64. */
int eaf_wav_open(eaf_wav_t *wav, eaf_source_t *source, const eaf_reader_t *reader);
