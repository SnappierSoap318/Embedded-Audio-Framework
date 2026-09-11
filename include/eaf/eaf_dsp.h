#pragma once
#include <eaf/eaf_core.h>
#define EAF_EQ_MAX_BANDS 8u
typedef struct {
    int32_t b0, b1, b2, a1, a2;
} eaf_biquad_coeff_t;
typedef struct {
    int32_t x1, x2, y1, y2;
} eaf_biquad_state_t;
int32_t eaf_q31_multiply(int32_t sample, int32_t gain);
int32_t eaf_biquad_tick(const eaf_biquad_coeff_t *c, eaf_biquad_state_t *s, int32_t x);
int eaf_biquad_lowpass(eaf_biquad_coeff_t *c, double rate, double hz);
int eaf_biquad_highpass(eaf_biquad_coeff_t *c, double rate, double hz);
/* Gains in [0, INT32_MAX]. INT32_MAX is treated as exact unity. */
typedef struct {
    int32_t gain[EAF_MAX_CHANNELS];
} eaf_volume_ctx_t;
typedef struct {
    size_t bands;
    eaf_biquad_coeff_t coeff[EAF_EQ_MAX_BANDS];
    eaf_biquad_state_t state[EAF_MAX_CHANNELS][EAF_EQ_MAX_BANDS];
} eaf_eq_ctx_t;
typedef struct {
    double frequency_hz;
    eaf_biquad_coeff_t lp, hp;
    eaf_biquad_state_t low[2], high[2][2];
} eaf_crossover_2_1_ctx_t;
extern const struct eaf_node_ops eaf_volume_ops;
extern const struct eaf_node_ops eaf_biquad_eq_ops;
extern const struct eaf_node_ops eaf_crossover_2_1_ops;
