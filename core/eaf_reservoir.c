#include <eaf/eaf_reservoir.h>
#include <string.h>
int eaf_reservoir_init(eaf_reservoir_t *r, int32_t *storage, uint32_t capacity, eaf_format_t fmt,
                       uint32_t high_watermark) {
    if (!r || !storage || !eaf_format_valid(&fmt) || capacity < 2u || capacity > UINT32_MAX / 2u ||
        (capacity & (capacity - 1u)) || !high_watermark || high_watermark > capacity ||
        capacity > SIZE_MAX / sizeof(int32_t) / fmt.num_channels)
        return EAF_INVALID;
    *r = (eaf_reservoir_t){.storage = storage,
                           .capacity = capacity,
                           .high_watermark = high_watermark,
                           .format = fmt,
                           .backpressure_low = capacity / 2u,
                           .backpressure_high = capacity - capacity / 10u};
    atomic_init(&r->read_cursor.value, 0u);
    atomic_init(&r->write_cursor.value, 0u);
    atomic_init(&r->finished.value, 0u);
    eaf_reservoir_reset(r);
    return EAF_OK;
}
void eaf_reservoir_reset(eaf_reservoir_t *r) {
    hal_atomic_set(&r->read_cursor, 0);
    hal_atomic_set(&r->write_cursor, 0);
    hal_atomic_set(&r->finished, 0);
    r->state = EAF_RESERVOIR_PREBUFFERING;
    r->underruns = 0;
    r->frames_read = 0;
    memset(r->last, 0, sizeof(r->last));
}
uint32_t eaf_reservoir_level(const eaf_reservoir_t *r) {
    uint32_t read = hal_atomic_get(&r->read_cursor);
    uint32_t write = hal_atomic_get(&r->write_cursor);
    uint32_t level = write - read;
    return level > r->capacity ? r->capacity : level;
}
bool eaf_reservoir_backpressure(const eaf_reservoir_t *r) {
    return eaf_reservoir_level(r) >= r->backpressure_high;
}
uint32_t eaf_reservoir_write(eaf_reservoir_t *r, const int32_t *src, uint32_t frames) {
    if (!r || !src || hal_atomic_get(&r->finished))
        return 0;
    uint32_t write = hal_atomic_get(&r->write_cursor);
    uint32_t available = r->capacity - (write - hal_atomic_get(&r->read_cursor));
    if (frames > available)
        frames = available;
    size_t channels = r->format.num_channels;
    for (uint32_t i = 0; i < frames; ++i) {
        size_t slot = (write + i) & (r->capacity - 1u);
        memcpy(r->storage + slot * channels, src + (size_t)i * channels, channels * sizeof(*src));
    }
    hal_atomic_set(&r->write_cursor, write + frames);
    return frames;
}
void eaf_reservoir_finish(eaf_reservoir_t *r) {
    if (r)
        hal_atomic_set(&r->finished, 1);
}
int eaf_reservoir_pull(eaf_reservoir_t *r, eaf_buffer_t *buf) {
    if (!r || !buf || !buf->samples || !buf->frame_count ||
        buf->frame_count > buf->capacity_frames || buf->frame_count > r->capacity ||
        buf->capacity_samples / r->format.num_channels < buf->frame_count)
        return EAF_INVALID;
    /* Acquire EOF before the cursor: observing EOF must include the final write. */
    bool finished = hal_atomic_get(&r->finished) != 0;
    uint32_t read = hal_atomic_get(&r->read_cursor);
    uint32_t available = hal_atomic_get(&r->write_cursor) - read;
    uint32_t n = buf->frame_count;
    size_t channels = r->format.num_channels;
    buf->format = r->format;
    buf->flags = 0;
    if (finished) {
        uint32_t take = available < n ? available : n;
        for (uint32_t i = 0; i < take; ++i) {
            size_t slot = (read + i) & (r->capacity - 1u);
            memcpy(buf->samples + (size_t)i * channels, r->storage + slot * channels,
                   channels * sizeof(int32_t));
        }
        memset(buf->samples + (size_t)take * channels, 0,
               (size_t)(n - take) * channels * sizeof(int32_t));
        hal_atomic_set(&r->read_cursor, read + take);
        r->frames_read += take;
        r->state = available <= n ? EAF_RESERVOIR_DRAINED : EAF_RESERVOIR_STREAMING;
        if (!take)
            buf->flags |= EAF_FRAME_SILENCE;
        if (r->state == EAF_RESERVOIR_DRAINED)
            buf->flags |= EAF_FRAME_EOS;
        return EAF_OK;
    }
    if (r->state == EAF_RESERVOIR_UNDERRUN)
        r->state = EAF_RESERVOIR_STREAMING;
    if (r->state == EAF_RESERVOIR_PREBUFFERING && available >= r->high_watermark && available >= n)
        r->state = EAF_RESERVOIR_STREAMING;
    if (r->state == EAF_RESERVOIR_STREAMING) {
        uint32_t take = available < n ? available : n;
        for (uint32_t i = 0; i < take; ++i) {
            size_t slot = (read + i) & (r->capacity - 1u);
            memcpy(buf->samples + (size_t)i * channels, r->storage + slot * channels,
                   channels * sizeof(int32_t));
        }
        if (take < n) {
            /* Preserve queued frames and pad the shortfall with a short ramp from
               the last real sample. A mid-stream underrun resumes on the next pull
               instead of discarding the tail and refilling to the watermark. */
            int32_t from[EAF_MAX_CHANNELS];
            if (take)
                memcpy(from, buf->samples + (size_t)(take - 1u) * channels,
                       channels * sizeof(int32_t));
            else
                memcpy(from, r->last, channels * sizeof(int32_t));
            memset(buf->samples + (size_t)take * channels, 0,
                   (size_t)(n - take) * channels * sizeof(int32_t));
            uint32_t fade = (n - take) < 16u ? (n - take) : 16u;
            for (uint32_t i = 0; i < fade; ++i)
                for (size_t ch = 0; ch < channels; ++ch)
                    buf->samples[(size_t)(take + i) * channels + ch] =
                        (int32_t)((int64_t)from[ch] * (int32_t)(fade - i - 1u) / (int32_t)fade);
            memset(r->last, 0, sizeof(r->last));
            r->state = EAF_RESERVOIR_UNDERRUN;
            ++r->underruns;
            buf->flags = EAF_FRAME_UNDERRUN;
        } else {
            memcpy(r->last, buf->samples + (size_t)(n - 1u) * channels, channels * sizeof(int32_t));
        }
        hal_atomic_set(&r->read_cursor, read + take);
        r->frames_read += take;
        available -= take;
    } else {
        memset(buf->samples, 0, (size_t)n * channels * sizeof(int32_t));
        buf->flags = EAF_FRAME_SILENCE;
    }
    if (available < r->backpressure_low && r->wake_producer)
        r->wake_producer(r->wake_ctx);
    return EAF_OK;
}
