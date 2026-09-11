#include "check.h"
#include <eaf/eaf_reservoir.h>
int main(void) {
    eaf_reservoir_t r;
    int32_t storage[64], src[64], out[32];
    for (size_t i = 0; i < 64; ++i)
        src[i] = 1600 + (int32_t)i;
    eaf_format_t fmt = {48000, 2, 3};
    eaf_buffer_t b = {out, 16, 16, 32, fmt, 0};
    CHECK(eaf_reservoir_init(&r, storage, 31, fmt, 24) == EAF_INVALID);
    CHECK(eaf_reservoir_init(&r, storage, 32, fmt, 24) == 0);
    CHECK(eaf_reservoir_write(&r, src, 16) == 16);
    CHECK(eaf_reservoir_pull(&r, &b) == 0 && b.flags == EAF_FRAME_SILENCE);
    CHECK(eaf_reservoir_level(&r) == 16);
    for (size_t i = 0; i < 32; ++i)
        CHECK(out[i] == 0);
    CHECK(eaf_reservoir_write(&r, src + 32, 16) == 16);
    CHECK(eaf_reservoir_write(&r, src, 1) == 0);
    CHECK(eaf_reservoir_backpressure(&r));
    CHECK(eaf_reservoir_pull(&r, &b) == 0 && r.state == EAF_RESERVOIR_STREAMING);
    for (size_t i = 0; i < 32; ++i)
        CHECK(out[i] == src[i]);
    CHECK(eaf_reservoir_pull(&r, &b) == 0);
    for (size_t i = 0; i < 32; ++i)
        CHECK(out[i] == src[i + 32]);
    CHECK(eaf_reservoir_pull(&r, &b) == 0 && r.state == EAF_RESERVOIR_UNDERRUN);
    CHECK(r.underruns == 1 && b.flags == EAF_FRAME_UNDERRUN);
    for (size_t i = 0; i < 16; ++i) {
        CHECK(out[i * 2] == src[62] * (int32_t)(15u - i) / 16);
        CHECK(out[i * 2 + 1] == src[63] * (int32_t)(15u - i) / 16);
    }
    CHECK(eaf_reservoir_write(&r, src, 16) == 16);
    CHECK(eaf_reservoir_pull(&r, &b) == 0 && r.state == EAF_RESERVOIR_PREBUFFERING);
    CHECK(eaf_reservoir_level(&r) == 16 && r.underruns == 1);
    CHECK(eaf_reservoir_write(&r, src + 32, 8) == 8);
    CHECK(eaf_reservoir_pull(&r, &b) == 0 && r.state == EAF_RESERVOIR_STREAMING);
    CHECK(eaf_reservoir_pull(&r, &b) == 0 && r.underruns == 2);
    CHECK(eaf_reservoir_level(&r) == 0); /* partial tail discarded */
    eaf_reservoir_reset(&r);
    hal_atomic_set(&r.read_cursor, UINT32_MAX - 7u);
    hal_atomic_set(&r.write_cursor, UINT32_MAX - 7u);
    CHECK(eaf_reservoir_write(&r, src, 32) == 32);
    CHECK(eaf_reservoir_pull(&r, &b) == 0);
    for (size_t i = 0; i < 32; ++i)
        CHECK(out[i] == src[i]);
    CHECK(eaf_reservoir_pull(&r, &b) == 0);
    for (size_t i = 0; i < 32; ++i)
        CHECK(out[i] == src[i + 32]);
    b.capacity_samples = 1;
    CHECK(eaf_reservoir_pull(&r, &b) == EAF_INVALID);
    CHECK(eaf_pcm16_to_q31(INT16_MIN) == INT32_MIN);
    CHECK(eaf_pcm24_to_q31(-8388608) == INT32_MIN);
    CHECK(eaf_pcm24_to_q31(INT32_MAX) == INT32_MAX);
    /* EOF bypasses prebuffering and preserves a partial final block. */
    eaf_reservoir_reset(&r);
    b.capacity_samples = 32;
    CHECK(eaf_reservoir_write(&r, src, 3) == 3);
    eaf_reservoir_finish(&r);
    CHECK(eaf_reservoir_write(&r, src, 1) == 0);
    CHECK(eaf_reservoir_pull(&r, &b) == 0 && (b.flags & EAF_FRAME_EOS));
    CHECK(r.frames_read == 3 && !r.underruns && r.state == EAF_RESERVOIR_DRAINED);
    for (size_t i = 0; i < 6; ++i)
        CHECK(out[i] == src[i]);
    for (size_t i = 6; i < 32; ++i)
        CHECK(out[i] == 0);
    /* Repeated EOF is silent and stable; reset reopens the producer endpoint. */
    CHECK(eaf_reservoir_pull(&r, &b) == 0 && b.flags == (EAF_FRAME_SILENCE | EAF_FRAME_EOS));
    CHECK(r.frames_read == 3 && !r.underruns);
    eaf_reservoir_reset(&r);
    CHECK(eaf_reservoir_write(&r, src, 32) == 32);
    eaf_reservoir_finish(&r);
    CHECK(eaf_reservoir_pull(&r, &b) == 0 && !(b.flags & EAF_FRAME_EOS));
    CHECK(eaf_reservoir_pull(&r, &b) == 0 && (b.flags & EAF_FRAME_EOS));
    CHECK(r.frames_read == 32 && !r.underruns);
    return 0;
}
