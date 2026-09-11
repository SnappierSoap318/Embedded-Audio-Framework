#include <eaf/eaf_dsp.h>
#include <math.h>
#include <string.h>
static int32_t sat(int64_t x) {
    if (x > INT32_MAX)
        return INT32_MAX;
    if (x < INT32_MIN)
        return INT32_MIN;
    return (int32_t)x;
}
int32_t eaf_q31_multiply(int32_t sample, int32_t gain) {
    if (gain == INT32_MAX)
        return sample;
    return sat((int64_t)sample * gain / INT64_C(2147483648));
}
int32_t eaf_biquad_tick(const eaf_biquad_coeff_t *c, eaf_biquad_state_t *s, int32_t x) {
    /* Each product / 4 is bounded by 2^60; all five fit in int64_t.
       Division has defined rounding toward zero, including negative values. */
    int64_t acc = (int64_t)c->b0 * x / 4 + (int64_t)c->b1 * s->x1 / 4 + (int64_t)c->b2 * s->x2 / 4 -
                  (int64_t)c->a1 * s->y1 / 4 - (int64_t)c->a2 * s->y2 / 4;
    int32_t y = sat(acc / INT64_C(268435456));
    s->x2 = s->x1;
    s->x1 = x;
    s->y2 = s->y1;
    s->y1 = y;
    return y;
}
static int design(eaf_biquad_coeff_t *c, double rate, double hz, bool high) {
    if (!c || !isfinite(rate) || !isfinite(hz) || rate <= 0.0 || hz <= 0.0 || hz >= rate / 2.0)
        return EAF_INVALID;
    double w = 6.2831853071795864769 * hz / rate;
    double cs = cos(w), alpha = sin(w) / sqrt(2.0), a0 = 1.0 + alpha;
    double b0 = (high ? 1.0 + cs : 1.0 - cs) / (2.0 * a0);
    double values[5] = {b0, (high ? -2.0 : 2.0) * b0, b0, -2.0 * cs / a0, (1.0 - alpha) / a0};
    int32_t q[5];
    for (size_t i = 0; i < 5; ++i) {
        double scaled = round(values[i] * 1073741824.0);
        if (scaled < (double)INT32_MIN || scaled > (double)INT32_MAX)
            return EAF_INVALID;
        q[i] = (int32_t)scaled;
    }
    *c = (eaf_biquad_coeff_t){q[0], q[1], q[2], q[3], q[4]};
    return EAF_OK;
}
int eaf_biquad_lowpass(eaf_biquad_coeff_t *c, double rate, double hz) {
    return design(c, rate, hz, false);
}
int eaf_biquad_highpass(eaf_biquad_coeff_t *c, double rate, double hz) {
    return design(c, rate, hz, true);
}
static bool valid_buffer(const eaf_buffer_t *b) {
    return b && b->samples && eaf_format_valid(&b->format) &&
           b->frame_count <= b->capacity_frames &&
           b->frame_count <= b->capacity_samples / b->format.num_channels;
}
static void noop_deinit(eaf_node_t *n) {
    (void)n;
}
static int noop_reset(eaf_node_t *n) {
    (void)n;
    return EAF_OK;
}
static int volume_init(eaf_node_t *n, const eaf_format_t *in, eaf_format_t *out) {
    if (!n->ctx || !eaf_format_valid(in))
        return EAF_INVALID;
    eaf_volume_ctx_t *v = n->ctx;
    for (size_t ch = 0; ch < in->num_channels; ++ch)
        if (v->gain[ch] < 0)
            return EAF_INVALID;
    *out = *in;
    return EAF_OK;
}
static int volume_process(eaf_node_t *n, eaf_buffer_t *b) {
    if (!valid_buffer(b))
        return EAF_INVALID;
    eaf_volume_ctx_t *v = n->ctx;
    for (uint32_t i = 0; i < b->frame_count; ++i)
        for (size_t ch = 0; ch < b->format.num_channels; ++ch) {
            size_t slot = (size_t)i * b->format.num_channels + ch;
            b->samples[slot] = eaf_q31_multiply(b->samples[slot], v->gain[ch]);
        }
    return EAF_OK;
}
const struct eaf_node_ops eaf_volume_ops = {volume_init, volume_process, noop_reset, noop_deinit};
static int eq_reset(eaf_node_t *n) {
    eaf_eq_ctx_t *e = n->ctx;
    memset(e->state, 0, sizeof(e->state));
    return EAF_OK;
}
static int eq_init(eaf_node_t *n, const eaf_format_t *in, eaf_format_t *out) {
    eaf_eq_ctx_t *e = n->ctx;
    if (!e || e->bands > EAF_EQ_MAX_BANDS || !eaf_format_valid(in))
        return EAF_INVALID;
    *out = *in;
    return eq_reset(n);
}
static int eq_process(eaf_node_t *n, eaf_buffer_t *b) {
    if (!valid_buffer(b))
        return EAF_INVALID;
    eaf_eq_ctx_t *e = n->ctx;
    for (uint32_t i = 0; i < b->frame_count; ++i)
        for (size_t ch = 0; ch < b->format.num_channels; ++ch) {
            size_t slot = (size_t)i * b->format.num_channels + ch;
            for (size_t band = 0; band < e->bands; ++band)
                b->samples[slot] =
                    eaf_biquad_tick(&e->coeff[band], &e->state[ch][band], b->samples[slot]);
        }
    /* Filter tails may be audible even when source block is silent. */
    b->flags &= ~EAF_FRAME_SILENCE;
    return EAF_OK;
}
const struct eaf_node_ops eaf_biquad_eq_ops = {eq_init, eq_process, eq_reset, noop_deinit};
static int cross_reset(eaf_node_t *n) {
    eaf_crossover_2_1_ctx_t *c = n->ctx;
    memset(c->low, 0, sizeof(c->low));
    memset(c->high, 0, sizeof(c->high));
    return EAF_OK;
}
static int cross_init(eaf_node_t *n, const eaf_format_t *in, eaf_format_t *out) {
    eaf_crossover_2_1_ctx_t *c = n->ctx;
    if (!c || !eaf_format_valid(in) || in->num_channels != 2u || in->channel_mask != 3u)
        return EAF_INVALID;
    int rc = eaf_biquad_lowpass(&c->lp, in->sample_rate, c->frequency_hz);
    if (!rc)
        rc = eaf_biquad_highpass(&c->hp, in->sample_rate, c->frequency_hz);
    if (rc)
        return rc;
    *out = (eaf_format_t){in->sample_rate, 3, 7};
    return cross_reset(n);
}
static int cross_process(eaf_node_t *n, eaf_buffer_t *b) {
    if (!valid_buffer(b) || b->format.num_channels != 2u || b->format.channel_mask != 3u ||
        b->capacity_samples / 3u < b->frame_count)
        return EAF_INVALID;
    eaf_crossover_2_1_ctx_t *c = n->ctx;
    for (size_t i = b->frame_count; i > 0; --i) {
        size_t j = i - 1u;
        int32_t l = b->samples[j * 2u], r = b->samples[j * 2u + 1u];
        b->samples[j * 3u] = l;
        b->samples[j * 3u + 1u] = r;
        b->samples[j * 3u + 2u] = (int32_t)(((int64_t)l + r) / 2);
    }
    for (size_t i = 0; i < b->frame_count; ++i) {
        for (size_t stage = 0; stage < 2; ++stage) {
            for (size_t ch = 0; ch < 2; ++ch)
                b->samples[i * 3u + ch] =
                    eaf_biquad_tick(&c->hp, &c->high[ch][stage], b->samples[i * 3u + ch]);
            b->samples[i * 3u + 2u] =
                eaf_biquad_tick(&c->lp, &c->low[stage], b->samples[i * 3u + 2u]);
        }
    }
    b->format.num_channels = 3;
    b->format.channel_mask = 7;
    b->flags &= ~EAF_FRAME_SILENCE;
    return EAF_OK;
}
const struct eaf_node_ops eaf_crossover_2_1_ops = {cross_init, cross_process, cross_reset,
                                                   noop_deinit};
