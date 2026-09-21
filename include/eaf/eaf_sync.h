#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Clock-synchronization primitives for multiroom playback. A bounded PI
   controller turns a presentation-timing error into a clock-rate correction,
   and a linear resampler applies a fractional rate change to one block.
   Portable: no OS dependencies, double arithmetic only in control/config. */

typedef struct {
    double kp, ki, limit_ppm;
    double integral;
    double output_ppm;
} eaf_sync_controller_t;

void eaf_sync_controller_init(eaf_sync_controller_t *c, double kp, double ki, double limit_ppm);
void eaf_sync_controller_reset(eaf_sync_controller_t *c);
/* error_ms is the target minus the measured value, so a positive error means
   the controlled quantity must increase; dt_s is the time since the previous
   call in seconds. Returns the bounded correction in ppm. The caller decides
   what the sign means (for playback, positive increases the output rate). */
double eaf_sync_controller_update(eaf_sync_controller_t *c, double error_ms, double dt_s);

/* Output/input frame ratio for a ppm correction: 1 + ppm*1e-6. */
double eaf_sync_ppm_to_ratio(double ppm);

/* Linear-interpolation resampler. Output sample 0 maps to input sample 0, so a
   fixed output block is produced from a slightly larger or smaller input block
   with no state between calls; adjacent blocks stay continuous because the
   mapping restarts just after the previous block's last input sample.
   `channels` is 1 or 2. Returns out_frames, or 0 for invalid arguments. */
uint32_t eaf_sync_resample(const int32_t *in, uint32_t in_frames, int32_t *out, uint32_t out_frames,
                           uint8_t channels);
