#include "check.h"
#include <eaf/eaf_dsp.h>
#include <float.h>
#include <math.h>
#include <string.h>
typedef struct {
    double x1, x2, y1, y2;
} reference_state;
static double reference(const double c[5], reference_state *s, double x) {
    double y = c[0] * x + c[1] * s->x1 + c[2] * s->x2 - c[3] * s->y1 - c[4] * s->y2;
    s->x2 = s->x1;
    s->x1 = x;
    s->y2 = s->y1;
    s->y1 = y;
    return y;
}
static void coefficients(double c[5], double hz, bool high) {
    double w = 6.2831853071795864769 * hz / 48000.0;
    double a = 1.0 + sin(w) / sqrt(2.0);
    c[0] = (high ? 1.0 + cos(w) : 1.0 - cos(w)) / (2.0 * a);
    c[1] = (high ? -2.0 : 2.0) * c[0];
    c[2] = c[0];
    c[3] = -2.0 * cos(w) / a;
    c[4] = (2.0 - a) / a;
}
int main(void) {
    /* Finite extreme values must normalize before multiplying by 2*pi. */
    eaf_biquad_coeff_t extreme, normalized;
    CHECK(eaf_biquad_lowpass(&extreme, DBL_MAX, DBL_MAX / 4.0) == 0);
    CHECK(eaf_biquad_lowpass(&normalized, 48000, 12000) == 0);
    CHECK(extreme.b0 == normalized.b0 && extreme.b1 == normalized.b1 &&
          extreme.b2 == normalized.b2 && extreme.a1 == normalized.a1 &&
          extreme.a2 == normalized.a2);

    CHECK(eaf_q31_multiply(INT32_MIN, INT32_MIN) == INT32_MAX);
    CHECK(eaf_q31_multiply(INT32_MIN, INT32_MAX) == INT32_MIN);
    CHECK(eaf_q31_multiply(INT32_MAX, 0) == 0);
    CHECK(eaf_q31_multiply(INT32_MIN, 1073741824) == -1073741824);
    eaf_biquad_coeff_t q;
    CHECK(eaf_biquad_lowpass(&q, 48000, (double)NAN) == EAF_INVALID);
    CHECK(eaf_biquad_lowpass(&q, 48000, 24000) == EAF_INVALID);
    CHECK(eaf_biquad_lowpass(&q, 48000, 80) == 0);
    double lp[5], hp[5];
    coefficients(lp, 80, false);
    coefficients(hp, 80, true);
    reference_state ref = {0};
    eaf_biquad_state_t state = {0};
    double max_error = 0;
    for (unsigned i = 0; i < 48000; ++i) {
        int32_t x = (int32_t)(sin((double)i * 0.011) * 536870912.0);
        double expected = reference(lp, &ref, (double)x);
        double error = fabs((double)eaf_biquad_tick(&q, &state, x) - expected);
        if (error > max_error)
            max_error = error;
    }
    printf("80 Hz biquad max error: %.1f Q31 counts\n", max_error);
    CHECK(max_error < 20000.0);
    /* All extreme products must remain defined and output must saturate. */
    q = (eaf_biquad_coeff_t){INT32_MIN, INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX};
    state = (eaf_biquad_state_t){INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN};
    CHECK(eaf_biquad_tick(&q, &state, INT32_MIN) == INT32_MAX);
    eaf_crossover_2_1_ctx_t ctx = {.frequency_hz = 80};
    eaf_node_t node = {"cross", EAF_NODE_STAGE_PRE_PROCESS, &eaf_crossover_2_1_ops, &ctx};
    eaf_format_t in = {48000, 2, 3}, out;
    CHECK(node.ops->init(&node, &in, &out) == 0 && out.num_channels == 3);
    reference_state refs[3][2] = {0};
    int32_t samples[(size_t)128 * 3 + 1];
    samples[(size_t)128 * 3] = 123456;
    eaf_buffer_t b = {samples, 128, 128, (size_t)128 * 3, in, 0};
    max_error = 0;
    for (size_t block = 0; block < 100; ++block) {
        double expected[128][3];
        for (size_t i = 0; i < 128; ++i) {
            size_t t = block * 128 + i;
            int32_t l = (int32_t)(sin((double)t * 0.01) * 268435456.0);
            int32_t r = (int32_t)(cos((double)t * 0.03) * 134217728.0);
            samples[i * 2] = l;
            samples[i * 2 + 1] = r;
            /* Match the integer downmix rounding used by the DSP. */
            // NOLINTNEXTLINE(bugprone-integer-division)
            double v[3] = {(double)l, (double)r, (double)(((int64_t)l + r) / 2)};
            for (size_t ch = 0; ch < 3; ++ch) {
                for (size_t stage = 0; stage < 2; ++stage)
                    v[ch] = reference(ch == 2 ? lp : hp, &refs[ch][stage], v[ch]);
                expected[i][ch] = v[ch];
            }
        }
        b.format = in;
        CHECK(node.ops->process(&node, &b) == 0);
        CHECK(samples[(size_t)128 * 3] == 123456);
        for (size_t i = 0; i < 128; ++i)
            for (size_t ch = 0; ch < 3; ++ch) {
                double error = fabs((double)samples[i * 3 + ch] - expected[i][ch]);
                if (error > max_error)
                    max_error = error;
            }
    }
    printf("LR4 max error: %.1f Q31 counts\n", max_error);
    CHECK(max_error < 40000.0);
    b.format = in;
    b.capacity_samples = 256;
    CHECK(node.ops->process(&node, &b) == EAF_INVALID);
    CHECK(node.ops->reset(&node) == 0);
    CHECK(ctx.high[0][0].y1 == 0 && ctx.low[1].y1 == 0);
    return 0;
}
