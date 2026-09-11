#define _POSIX_C_SOURCE 200809L
#include "check.h"
#include <alsa/asoundlib.h>
#include <eaf/eaf_sink_alsa.h>
#include <errno.h>
static bool fail_write, fail_drop, fail_drain, timeout_drain, fake_clock;
static uint64_t clock_us;
static unsigned drains;
snd_pcm_sframes_t __real_snd_pcm_writei(snd_pcm_t *, const void *, snd_pcm_uframes_t);
int __real_snd_pcm_drop(snd_pcm_t *);
int __real_snd_pcm_drain(snd_pcm_t *);
uint64_t __real_hal_monotonic_time_us(void);
snd_pcm_sframes_t __wrap_snd_pcm_writei(snd_pcm_t *p, const void *b, snd_pcm_uframes_t n) {
    return fail_write ? -EIO : __real_snd_pcm_writei(p, b, n);
}
int __wrap_snd_pcm_drop(snd_pcm_t *p) {
    return fail_drop ? -EIO : __real_snd_pcm_drop(p);
}
int __wrap_snd_pcm_drain(snd_pcm_t *p) {
    ++drains;
    if (timeout_drain)
        return -EAGAIN;
    return fail_drain ? -EIO : __real_snd_pcm_drain(p);
}
uint64_t __wrap_hal_monotonic_time_us(void) {
    if (fake_clock) {
        clock_us += 100000u;
        return clock_us;
    }
    return __real_hal_monotonic_time_us();
}
static eaf_alsa_sink_ctx_t ctx;
int main(void) {
    ctx.device = "null";
    eaf_sink_t s = {&eaf_alsa_sink_ops, &ctx};
    eaf_format_t f = {48000, 2, 3};
    CHECK(!s.ops->init(&s, &f, 16));
    CHECK(!s.ops->start(&s));
    eaf_buffer_t *b;
    CHECK(!s.ops->acquire_buf(&s, &b));
    fail_write = true;
    CHECK(s.ops->commit_buf(&s, b) == EAF_IO);
    CHECK(!ctx.acquired && ctx.frames_written == 0);
    fail_write = false;
    CHECK(!s.ops->acquire_buf(&s, &b));
    b->flags = EAF_FRAME_EOS;
    fail_drain = true;
    CHECK(s.ops->commit_buf(&s, b) == EAF_IO);
    CHECK(drains == 1 && ctx.frames_written == 16);
    fail_drain = false;
    timeout_drain = fake_clock = true;
    CHECK(eaf_alsa_drain(&ctx) == EAF_IO);
    CHECK(clock_us == 1100000u && drains == 11);
    timeout_drain = fake_clock = false;
    fail_drop = true;
    CHECK(s.ops->stop(&s) == EAF_IO);
    CHECK(s.ops->deinit(&s) == EAF_IO);
    CHECK(ctx.pcm && ctx.running);
    fail_drop = false;
    CHECK(!s.ops->stop(&s));
    CHECK(!s.ops->deinit(&s));
    CHECK(!ctx.pcm && !ctx.running);
    CHECK(!s.ops->deinit(&s));
    return 0;
}
