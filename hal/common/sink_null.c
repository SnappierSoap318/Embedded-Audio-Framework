#include <eaf/eaf_sink_null.h>
static int init(eaf_sink_t *sink, const eaf_format_t *fmt, size_t frames) {
    if (!sink->driver_data || !eaf_format_valid(fmt) || !frames || frames > EAF_NULL_MAX_FRAMES)
        return EAF_INVALID;
    eaf_null_sink_ctx_t *s = sink->driver_data;
    if (s->running)
        return EAF_STATE;
    for (size_t i = 0; i < 2; ++i)
        s->buffers[i] =
            (eaf_buffer_t){.samples = s->samples[i],
                           .frame_count = (uint32_t)frames,
                           .capacity_frames = EAF_NULL_MAX_FRAMES,
                           .capacity_samples = (size_t)EAF_NULL_MAX_FRAMES * EAF_MAX_CHANNELS,
                           .format = *fmt};
    s->active = 0;
    s->acquired = false;
    s->initialized = true;
    return EAF_OK;
}
static int start(eaf_sink_t *sink) {
    eaf_null_sink_ctx_t *s = sink->driver_data;
    if (!s->initialized || s->running)
        return EAF_STATE;
    s->epoch_us = hal_monotonic_time_us();
    s->frames_committed = 0;
    s->active = 0;
    s->acquired = false;
    s->running = true;
    return EAF_OK;
}
static int stop(eaf_sink_t *sink) {
    eaf_null_sink_ctx_t *s = sink->driver_data;
    s->running = false;
    s->acquired = false;
    return EAF_OK;
}
static int acquire(eaf_sink_t *sink, eaf_buffer_t **buf) {
    eaf_null_sink_ctx_t *s = sink->driver_data;
    if (!s->running || s->acquired || !buf)
        return EAF_STATE;
    *buf = &s->buffers[s->active];
    s->acquired = true;
    return EAF_OK;
}
static int commit(eaf_sink_t *sink, eaf_buffer_t *buf) {
    eaf_null_sink_ctx_t *s = sink->driver_data;
    if (!s->running || !s->acquired || buf != &s->buffers[s->active])
        return EAF_STATE;
    s->frames_committed += buf->frame_count;
    uint64_t rate = buf->format.sample_rate;
    uint64_t duration =
        s->frames_committed / rate * 1000000u + s->frames_committed % rate * 1000000u / rate;
    int rc = hal_sleep_until_us(s->epoch_us + duration);
    s->active ^= 1u;
    s->acquired = false;
    return rc;
}
static int adjust(eaf_sink_t *sink, int32_t ppm) {
    (void)sink;
    (void)ppm;
    return EAF_UNSUPPORTED;
}
static int deinit(eaf_sink_t *sink) {
    eaf_null_sink_ctx_t *s = sink->driver_data;
    if (s) {
        s->running = false;
        s->initialized = false;
        s->acquired = false;
    }
    return EAF_OK;
}
const struct eaf_sink_ops eaf_null_sink_ops = {init, start, stop, acquire, commit, adjust, deinit};
