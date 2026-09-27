#include "check.h"
#include <eaf/eaf_bt_decoder.h>
#include <eaf/eaf_bt_volume.h>

static int resets;
static int fake_reset(void *ctx) {
    (void)ctx;
    ++resets;
    return EAF_OK;
}

int main(void) {
    /* Gain ramps from silence to unity over 256 frames. */
    eaf_bt_volume_t gain = {0};
    int32_t scratch[2] = {1 << 20, 1 << 20};
    eaf_bt_volume_apply(&gain, scratch, 1, 127, false);
    CHECK(gain.target == 65536 && gain.current == 256 && gain.remaining == 255);
    for (int i = 0; i < 300; ++i)
        eaf_bt_volume_apply(&gain, scratch, 1, 127, false);
    CHECK(gain.current == 65536 && gain.remaining == 0);

    /* A settled unity gain is transparent to a fresh sample. */
    int32_t unity[2] = {1 << 20, -(1 << 20)};
    eaf_bt_volume_apply(&gain, unity, 1, 127, false);
    CHECK(unity[0] == (1 << 20) && unity[1] == -(1 << 20));

    /* Mute ramps down to silence and stops changing once settled. */
    eaf_bt_volume_apply(&gain, scratch, 1, 127, true);
    CHECK(gain.target == 0);
    for (int i = 0; i < 300; ++i)
        eaf_bt_volume_apply(&gain, scratch, 1, 127, true);
    CHECK(gain.current == 0 && gain.remaining == 0);
    int32_t muted[2] = {1 << 20, -(1 << 20)};
    eaf_bt_volume_apply(&gain, muted, 1, 127, true);
    CHECK(muted[0] == 0 && muted[1] == 0);

    /* Volume 0 is silent even when not muted; a mid volume scales but never inverts. */
    eaf_bt_volume_t zero = {0};
    int32_t samples[2] = {1000, -1000};
    eaf_bt_volume_apply(&zero, samples, 1, 0, false);
    CHECK(zero.target == 0 && samples[0] == 0 && samples[1] == 0);
    eaf_bt_volume_t half = {0};
    int32_t ramp[2] = {1000, -1000};
    eaf_bt_volume_apply(&half, ramp, 1, 64, false);
    CHECK(half.target > 0 && half.target < 65536);
    for (int i = 0; i < 300; ++i)
        eaf_bt_volume_apply(&half, ramp, 1, 64, false);
    CHECK(half.current == half.target && half.remaining == 0);
    int32_t wide[2] = {1000, -1000};
    eaf_bt_volume_apply(&half, wide, 1, 64, false);
    CHECK(wide[0] > 0 && wide[1] < 0 && wide[0] < 1000 && wide[1] > -1000);

    /* Out-of-range volume is clamped to the maximum, not wrapped. */
    eaf_bt_volume_t clamped = {0};
    int32_t pair[2] = {1 << 20, 1 << 20};
    eaf_bt_volume_apply(&clamped, pair, 1, 200, false);
    CHECK(clamped.target == 65536);

    /* Stream-boundary reset clears in-flight decode state and calls the codec reset. */
    eaf_bt_decoder_t decoder = {0};
    decoder.decoder.reset = fake_reset;
    decoder.pending = 5;
    decoder.sent = 2;
    decoder.frames_left = 3;
    decoder.offset = 7;
    resets = 0;
    CHECK(eaf_bt_decoder_reset(&decoder) == EAF_OK);
    CHECK(resets == 1 && decoder.pending == 0 && decoder.sent == 0 && decoder.frames_left == 0 &&
          decoder.offset == 0);
    CHECK(eaf_bt_decoder_reset(&decoder) == EAF_OK && resets == 2);

    /* A worker without a codec or a null pointer is rejected, not dereferenced. */
    eaf_bt_decoder_t empty = {0};
    CHECK(eaf_bt_decoder_reset(&empty) == EAF_INVALID);
    CHECK(eaf_bt_decoder_reset(NULL) == EAF_INVALID);
    return 0;
}
