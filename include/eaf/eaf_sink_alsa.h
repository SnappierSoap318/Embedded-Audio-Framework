#pragma once
#include <eaf/eaf_hal.h>
#define EAF_ALSA_MAX_FRAMES 1024u
typedef struct {
    /* Set before configure; NULL uses ALSA's default PCM. Borrowed lifetime. */
    const char *device;
    void *pcm;
    _Alignas(32) int32_t samples[EAF_ALSA_MAX_FRAMES * 2u];
    eaf_buffer_t buffer;
    uint64_t frames_written;
    uint32_t xruns;
    bool running, acquired, paused, pause_drained;
} eaf_alsa_sink_ctx_t;
extern const struct eaf_sink_ops eaf_alsa_sink_ops;
/* Output-owner calls only. Drain is bounded to 1 second; stop drops queued audio. */
int eaf_alsa_drain(eaf_alsa_sink_ctx_t *s);
int eaf_alsa_pause(eaf_alsa_sink_ctx_t *s, bool paused);
uint32_t eaf_alsa_delay(eaf_alsa_sink_ctx_t *s);
