#define _POSIX_C_SOURCE 200809L
#include <alsa/asoundlib.h>
#include <eaf/eaf_sink_alsa.h>
#include <errno.h>
static int init(eaf_sink_t *sink, const eaf_format_t *fmt, size_t frames) {
    eaf_alsa_sink_ctx_t *s = sink->driver_data;
    if (!s || s->pcm || !eaf_format_valid(fmt) || !frames || frames > EAF_ALSA_MAX_FRAMES ||
        !((fmt->num_channels == 1 && fmt->channel_mask == EAF_CH_FRONT_CENTER) ||
          (fmt->num_channels == 2 && fmt->channel_mask == 3)))
        return EAF_INVALID;
    snd_pcm_t *pcm = NULL;
    if (snd_pcm_open(&pcm, s->device ? s->device : "default", SND_PCM_STREAM_PLAYBACK,
                     SND_PCM_NONBLOCK) < 0)
        return EAF_IO;
    s->pcm = pcm; /* Retain even partial configuration for checked deinit. */
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S32, SND_PCM_ACCESS_RW_INTERLEAVED,
                           fmt->num_channels, fmt->sample_rate, 1, 50000) < 0) {
        return EAF_UNSUPPORTED;
    }
    s->pcm = pcm;
    s->buffer = (eaf_buffer_t){
        s->samples, (uint32_t)frames, EAF_ALSA_MAX_FRAMES, (size_t)EAF_ALSA_MAX_FRAMES * 2u, *fmt,
        0};
    return EAF_OK;
}
static int start(eaf_sink_t *sink) {
    eaf_alsa_sink_ctx_t *s = sink->driver_data;
    if (!s || !s->pcm || s->running)
        return EAF_STATE;
    if (snd_pcm_prepare(s->pcm) < 0)
        return EAF_IO;
    s->frames_written = 0;
    s->xruns = 0;
    s->running = true;
    s->acquired = false;
    s->paused = false;
    s->pause_drained = false;
    return EAF_OK;
}
static int stop(eaf_sink_t *sink) {
    eaf_alsa_sink_ctx_t *s = sink->driver_data;
    if (!s)
        return EAF_INVALID;
    if (!s->pcm)
        return EAF_OK;
    snd_pcm_state_t state = snd_pcm_state(s->pcm);
    int rc =
        (state == SND_PCM_STATE_OPEN || state == SND_PCM_STATE_SETUP) ? 0 : snd_pcm_drop(s->pcm);
    if (rc < 0)
        return EAF_IO;
    s->running = false;
    s->acquired = false;
    s->paused = false;
    return EAF_OK;
}
static int acquire(eaf_sink_t *sink, eaf_buffer_t **buf) {
    eaf_alsa_sink_ctx_t *s = sink->driver_data;
    if (!s || !s->running || s->acquired || s->paused || !buf)
        return EAF_STATE;
    s->acquired = true;
    *buf = &s->buffer;
    return EAF_OK;
}
static int commit(eaf_sink_t *sink, eaf_buffer_t *buf) {
    eaf_alsa_sink_ctx_t *s = sink->driver_data;
    if (!s || !s->running || !s->acquired || buf != &s->buffer ||
        buf->frame_count > EAF_ALSA_MAX_FRAMES)
        return EAF_STATE;
    uint32_t sent = 0;
    uint64_t deadline = hal_monotonic_time_us() + 250000u;
    int rc = EAF_OK;
    while (sent < buf->frame_count) {
        if (hal_monotonic_time_us() >= deadline) {
            rc = EAF_IO;
            break;
        }
        snd_pcm_sframes_t n =
            snd_pcm_writei(s->pcm, buf->samples + (size_t)sent * buf->format.num_channels,
                           buf->frame_count - sent);
        if (n > 0) {
            sent += (uint32_t)n;
            continue;
        }
        if (n == -EPIPE) {
            ++s->xruns;
            if (snd_pcm_prepare(s->pcm) >= 0)
                continue;
        }
        if (n == -EAGAIN || n == -EINTR || n == 0) {
            (void)snd_pcm_wait(s->pcm, 10);
            continue;
        }
        rc = EAF_IO;
        break;
    }
    s->frames_written += sent;
    s->acquired = false;
    if (!rc && (buf->flags & EAF_FRAME_EOS))
        rc = eaf_alsa_drain(s);
    return rc;
}
int eaf_alsa_drain(eaf_alsa_sink_ctx_t *s) {
    if (!s || !s->pcm || !s->running || s->paused)
        return EAF_STATE;
    uint64_t deadline = hal_monotonic_time_us() + 1000000u;
    int rc;
    while ((rc = snd_pcm_drain(s->pcm)) == -EAGAIN || rc == -EINTR) {
        if (hal_monotonic_time_us() >= deadline)
            return EAF_IO;
        hal_sleep_ms(1);
    }
    return rc < 0 ? EAF_IO : EAF_OK;
}
int eaf_alsa_pause(eaf_alsa_sink_ctx_t *s, bool paused) {
    if (!s || !s->pcm || !s->running)
        return EAF_STATE;
    if (s->paused == paused)
        return EAF_OK;
    if (paused) {
        s->pause_drained = snd_pcm_pause(s->pcm, 1) < 0;
        /* Plugins without pause finish their short queue before acknowledging. */
        if (s->pause_drained && eaf_alsa_drain(s))
            return EAF_IO;
    } else if ((s->pause_drained ? snd_pcm_prepare(s->pcm) : snd_pcm_pause(s->pcm, 0)) < 0)
        return EAF_IO;
    s->paused = paused;
    return EAF_OK;
}
uint32_t eaf_alsa_delay(eaf_alsa_sink_ctx_t *s) {
    snd_pcm_sframes_t delay = 0;
    if (!s || !s->pcm || snd_pcm_delay(s->pcm, &delay) < 0 || delay <= 0)
        return 0;
    return (uint64_t)delay > UINT32_MAX ? UINT32_MAX : (uint32_t)delay;
}
static int adjust(eaf_sink_t *sink, int32_t ppm) {
    (void)sink;
    (void)ppm;
    return EAF_UNSUPPORTED;
}
static int deinit(eaf_sink_t *sink) {
    eaf_alsa_sink_ctx_t *s = sink->driver_data;
    if (!s)
        return EAF_INVALID;
    if (!s->pcm)
        return EAF_OK;
    int rc = stop(sink);
    if (rc)
        return rc;
    /* ALSA close consumes the handle even if a plugin returns an error. */
    rc = snd_pcm_close(s->pcm);
    s->pcm = NULL;
    s->running = false;
    s->acquired = false;
    return rc < 0 ? EAF_IO : EAF_OK;
}
const struct eaf_sink_ops eaf_alsa_sink_ops = {init, start, stop, acquire, commit, adjust, deinit};
