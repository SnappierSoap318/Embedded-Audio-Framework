#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* Signal checks shared by the codec conformance tests. Goertzel is enough here:
   the fixtures are single-tone or sawtooth, so an FFT would be overkill. */

static inline double eaf_test_goertzel(const int32_t *pcm, uint32_t frames, uint32_t channels,
                                       uint32_t channel, uint32_t rate, double hz) {
    double w = 2.0 * 3.14159265358979323846 * hz / (double)rate;
    double c = 2.0 * cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (uint32_t i = 0; i < frames; ++i) {
        double x = (double)pcm[(size_t)i * channels + channel] / 2147483648.0;
        s0 = x + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

/* Strongest frequency over a coarse sweep. */
static inline double eaf_test_dominant_hz(const int32_t *pcm, uint32_t frames, uint32_t channels,
                                          uint32_t channel, uint32_t rate, double lo, double hi,
                                          double step) {
    double best = lo, best_power = -1.0;
    for (double hz = lo; hz <= hi; hz += step) {
        double power = eaf_test_goertzel(pcm, frames, channels, channel, rate, hz);
        if (power > best_power) {
            best_power = power;
            best = hz;
        }
    }
    return best;
}

/* RMS of one interleaved channel in Q1.31 units. */
static inline double eaf_test_rms(const int32_t *pcm, uint32_t frames, uint32_t channels,
                                  uint32_t channel) {
    double sum = 0.0;
    for (uint32_t i = 0; i < frames; ++i) {
        double x = (double)pcm[(size_t)i * channels + channel];
        sum += x * x;
    }
    return frames ? sqrt(sum / (double)frames) : 0.0;
}
