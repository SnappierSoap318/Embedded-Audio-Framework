#pragma once
#include <math.h>
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
    EAF_UNSUPPORTED = -4,
    EAF_TIMEOUT = -5
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
/* 32-bit PCM shares the Q1.31 range, so no scaling is required. */
static inline int32_t eaf_pcm32_to_q31(int32_t x) {
    return x;
}
/* Clamp to [-1, 1], scale and round to nearest. */
static inline int32_t eaf_float_to_q31(float x) {
    if (x >= 1.0f)
        return INT32_MAX;
    if (x <= -1.0f)
        return INT32_MIN;
    return (int32_t)lrintf(x * 2147483647.0f);
}
/* Widen/upmix Q1.31 samples; mono source duplicates into a stereo dest. */
static inline void eaf_q31_interleaved(const int32_t *src, uint32_t frames, uint8_t src_channels,
                                       int32_t *dst, uint8_t dst_channels) {
    for (uint32_t i = 0; i < frames; ++i) {
        int32_t left = src[(size_t)i * src_channels];
        if (dst_channels == 2u) {
            int32_t right = src_channels == 2u ? src[(size_t)i * src_channels + 1u] : left;
            dst[(size_t)i * 2u] = left;
            dst[(size_t)i * 2u + 1u] = right;
        } else {
            dst[i] = left;
        }
    }
}
/* Widen/upmix signed 16-bit samples to interleaved Q1.31. */
static inline void eaf_s16_to_q31_interleaved(const int16_t *src, uint32_t frames,
                                              uint8_t src_channels, int32_t *dst,
                                              uint8_t dst_channels) {
    for (uint32_t i = 0; i < frames; ++i) {
        int32_t left = eaf_pcm16_to_q31(src[(size_t)i * src_channels]);
        if (dst_channels == 2u) {
            int32_t right =
                src_channels == 2u ? eaf_pcm16_to_q31(src[(size_t)i * src_channels + 1u]) : left;
            dst[(size_t)i * 2u] = left;
            dst[(size_t)i * 2u + 1u] = right;
        } else {
            dst[i] = left;
        }
    }
}
