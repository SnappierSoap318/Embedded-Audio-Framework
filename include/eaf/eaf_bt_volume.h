#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Audio-owner-only gain state. Zero initialization starts silent. AVRCP's
 * 0..127 scale maps to squared amplitude; changes ramp over 256 stereo frames. */
typedef struct {
    uint32_t current, target, remaining;
} eaf_bt_volume_t;
void eaf_bt_volume_apply(eaf_bt_volume_t *gain, int32_t *stereo, size_t frames, uint8_t volume,
                         bool muted);
