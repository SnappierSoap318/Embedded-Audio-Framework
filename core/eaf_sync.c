#include <eaf/eaf_sync.h>
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

static int32_t interpolate(int32_t a, int32_t b, uint32_t fraction) {
    int64_t delta = (int64_t)b - (int64_t)a;
    int64_t product = delta * (int64_t)(fraction >> 1);
    if (product >= 0)
        return a + (int32_t)((product + INT64_C(1073741824)) >> 31);
    return a - (int32_t)((-product + INT64_C(1073741824)) >> 31);
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
    uint64_t step = ((uint64_t)in_frames << 32) / out_frames;
    for (uint32_t i = 0; i < out_frames; ++i) {
        uint64_t position = (uint64_t)i * step;
        uint32_t index = (uint32_t)(position >> 32);
        if (index >= in_frames - 1u) {
            for (uint8_t c = 0; c < channels; ++c)
                out[(size_t)i * channels + c] = in[(size_t)(in_frames - 1u) * channels + c];
            continue;
        }
        uint32_t fraction = (uint32_t)position;
        for (uint8_t c = 0; c < channels; ++c)
            out[(size_t)i * channels + c] =
                interpolate(in[(size_t)index * channels + c],
                            in[(size_t)(index + 1u) * channels + c], fraction);
    }
    return out_frames;
}
