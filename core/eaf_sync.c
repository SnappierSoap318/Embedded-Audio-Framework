#include <eaf/eaf_sync.h>
#include <math.h>
#include <stddef.h>

void eaf_sync_controller_init(eaf_sync_controller_t *c, double kp, double ki, double limit_ppm) {
    if (!c)
        return;
    *c = (eaf_sync_controller_t){.kp = kp, .ki = ki, .limit_ppm = limit_ppm};
}

void eaf_sync_controller_reset(eaf_sync_controller_t *c) {
    if (!c)
        return;
    c->integral = 0.0;
    c->output_ppm = 0.0;
}

double eaf_sync_controller_update(eaf_sync_controller_t *c, double error_ms, double dt_s) {
    if (!c)
        return 0.0;
    if (dt_s <= 0.0)
        return c->output_ppm;
    /* Clamp the integral before the sum so a long-lived error cannot wind it up
       past the actuator limit. */
    c->integral += c->ki * error_ms * dt_s;
    if (c->integral > c->limit_ppm)
        c->integral = c->limit_ppm;
    else if (c->integral < -c->limit_ppm)
        c->integral = -c->limit_ppm;
    c->output_ppm = c->kp * error_ms + c->integral;
    if (c->output_ppm > c->limit_ppm)
        c->output_ppm = c->limit_ppm;
    else if (c->output_ppm < -c->limit_ppm)
        c->output_ppm = -c->limit_ppm;
    return c->output_ppm;
}

double eaf_sync_ppm_to_ratio(double ppm) {
    return 1.0 + ppm * 1e-6;
}

static int32_t interpolate(int32_t a, int32_t b, double t) {
    double value = (double)a + ((double)b - (double)a) * t;
    return (int32_t)lround(value);
}

uint32_t eaf_sync_resample(const int32_t *in, uint32_t in_frames, int32_t *out, uint32_t out_frames,
                           uint8_t channels) {
    if (!in || !out || !in_frames || !out_frames || (channels != 1u && channels != 2u))
        return 0;
    if (in_frames == 1u) {
        for (uint32_t i = 0; i < out_frames; ++i)
            for (uint8_t c = 0; c < channels; ++c)
                out[(size_t)i * channels + c] = in[c];
        return out_frames;
    }
    double step = (double)in_frames / (double)out_frames;
    for (uint32_t i = 0; i < out_frames; ++i) {
        double position = (double)i * step;
        uint32_t index = (uint32_t)position;
        if (index >= in_frames - 1u) {
            for (uint8_t c = 0; c < channels; ++c)
                out[(size_t)i * channels + c] = in[(size_t)(in_frames - 1u) * channels + c];
            continue;
        }
        double fraction = position - (double)index;
        for (uint8_t c = 0; c < channels; ++c)
            out[(size_t)i * channels + c] =
                interpolate(in[(size_t)index * channels + c],
                            in[(size_t)(index + 1u) * channels + c], fraction);
    }
    return out_frames;
}
