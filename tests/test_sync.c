#include "check.h"
#include <eaf/eaf_sync.h>
#include <stdbool.h>

static bool near(double a, double b) {
    double diff = a - b;
    if (diff < 0.0)
        diff = -diff;
    return diff < 1e-9;
}

int main(void) {
    /* Proportional term, clamped to the actuator limit. */
    eaf_sync_controller_t c;
    eaf_sync_controller_init(&c, 1.0, 0.0, 50.0);
    CHECK(near(eaf_sync_controller_update(&c, 10.0, 1.0), 10.0));
    CHECK(near(eaf_sync_controller_update(&c, 100.0, 1.0), 50.0));
    CHECK(near(eaf_sync_controller_update(&c, 0.0, 1.0), 0.0));
    CHECK(near(eaf_sync_controller_update(&c, -100.0, 1.0), -50.0));

    /* Integral build-up with anti-windup, then recovery on the opposite error. */
    eaf_sync_controller_init(&c, 0.0, 2.0, 50.0);
    CHECK(near(eaf_sync_controller_update(&c, 10.0, 1.0), 20.0));
    CHECK(near(eaf_sync_controller_update(&c, 10.0, 1.0), 40.0));
    CHECK(near(eaf_sync_controller_update(&c, 10.0, 1.0), 50.0));
    CHECK(near(c.integral, 50.0));
    CHECK(near(eaf_sync_controller_update(&c, -10.0, 1.0), 30.0));

    /* A non-advancing step holds the last output. */
    double held = c.output_ppm;
    CHECK(near(eaf_sync_controller_update(&c, -10.0, 0.0), held));
    eaf_sync_controller_reset(&c);
    CHECK(near(c.output_ppm, 0.0) && near(c.integral, 0.0));

    CHECK(near(eaf_sync_ppm_to_ratio(0.0), 1.0));
    CHECK(near(eaf_sync_ppm_to_ratio(100.0), 1.0001));
    CHECK(near(eaf_sync_ppm_to_ratio(-100.0), 0.9999));

    /* Passthrough when the input and output frame counts match. */
    int32_t in[8] = {0, 10, 20, 30, 40, 50, 60, 70};
    int32_t out[8] = {0};
    CHECK(eaf_sync_resample(in, 4, out, 4, 1) == 4);
    CHECK(out[0] == 0 && out[1] == 10 && out[2] == 20 && out[3] == 30);

    /* Downsample 8 -> 4 consumes every second input sample. */
    CHECK(eaf_sync_resample(in, 8, out, 4, 1) == 4);
    CHECK(out[0] == 0 && out[1] == 20 && out[2] == 40 && out[3] == 60);

    /* Upsample 4 -> 8 interpolates and clamps at the last sample. */
    int32_t up_in[4] = {0, 20, 40, 60};
    CHECK(eaf_sync_resample(up_in, 4, out, 8, 1) == 8);
    CHECK(out[0] == 0 && out[1] == 10 && out[2] == 20 && out[3] == 30);
    CHECK(out[4] == 40 && out[5] == 50 && out[6] == 60 && out[7] == 60);

    /* Stereo interleaving is preserved. */
    int32_t stereo[4] = {1, 2, 3, 4};
    CHECK(eaf_sync_resample(stereo, 2, out, 2, 2) == 2);
    CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 4);

    /* Invalid arguments are rejected. */
    CHECK(eaf_sync_resample(NULL, 2, out, 2, 1) == 0);
    CHECK(eaf_sync_resample(in, 0, out, 2, 1) == 0);
    CHECK(eaf_sync_resample(in, 2, out, 2, 3) == 0);

    puts("sync PASS");
    return 0;
}
