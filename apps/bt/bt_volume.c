#include <eaf/eaf_bt_volume.h>
#include <eaf/eaf_dsp.h>

void eaf_bt_volume_apply(eaf_bt_volume_t *gain, int32_t *stereo, size_t frames, uint8_t volume,
                         bool muted) {
    uint32_t v = volume > 127u ? 127u : volume;
    uint32_t target = muted ? 0u : v * v * 65536u / (127u * 127u);
    if (target != gain->target) {
        gain->target = target;
        gain->remaining = 256;
    }
    for (size_t i = 0; i < frames; ++i) {
        if (gain->remaining) {
            int32_t delta = (int32_t)gain->target - (int32_t)gain->current;
            gain->current = (uint32_t)((int32_t)gain->current + delta / (int32_t)gain->remaining);
            --gain->remaining;
        }
        for (size_t ch = 0; ch < 2; ++ch)
            stereo[i * 2 + ch] = eaf_q16_multiply(stereo[i * 2 + ch], (int32_t)gain->current);
    }
}
