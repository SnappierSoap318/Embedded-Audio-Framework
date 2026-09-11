#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define EAF_MAX_CHANNELS 4u
#define EAF_FRAME_SILENCE 1u
#define EAF_FRAME_UNDERRUN 2u
#define EAF_FRAME_EOS 4u
typedef enum {
    EAF_EOF = 1,
    EAF_OK = 0,
    EAF_INVALID = -1,
    EAF_STATE = -2,
    EAF_IO = -3,
    EAF_UNSUPPORTED = -4
} eaf_result_t;
typedef enum {
    EAF_CH_FRONT_LEFT = 1u,
    EAF_CH_FRONT_RIGHT = 2u,
    EAF_CH_LFE = 4u,
    EAF_CH_FRONT_CENTER = 8u
} eaf_channel_mask_t;
typedef struct {
    uint32_t sample_rate;
    uint8_t num_channels;
    uint32_t channel_mask;
} eaf_format_t;
typedef struct {
    int32_t *samples;
    uint32_t frame_count;
    uint32_t capacity_frames;
    size_t capacity_samples;
    eaf_format_t format;
    uint32_t flags;
} eaf_buffer_t;
bool eaf_format_valid(const eaf_format_t *fmt);
bool eaf_format_equal(const eaf_format_t *a, const eaf_format_t *b);
/* Multiplication avoids undefined signed left shifts for negative PCM. */
static inline int32_t eaf_pcm16_to_q31(int16_t x) {
    return (int32_t)x * 65536;
}
/* Input must be sign-extended, in [-8388608, 8388607]. */
static inline int32_t eaf_pcm24_to_q31(int32_t x) {
    if (x > 8388607)
        return INT32_MAX;
    if (x < -8388608)
        return INT32_MIN;
    return x * 256;
}
