#pragma once
#include <eaf/eaf_hal.h>

/* Shared I2S DMA-ring sink state machine. Backends supply only device
   configuration and DMA block movement; ownership, pause gating and the
   retain-state-on-failure contract live here. */

#ifndef EAF_I2S_CORE_MAX_FRAMES
#ifdef CONFIG_EAF_I2S_MAX_FRAMES
#define EAF_I2S_CORE_MAX_FRAMES CONFIG_EAF_I2S_MAX_FRAMES
#else
#define EAF_I2S_CORE_MAX_FRAMES 1024u
#endif
#endif

typedef struct eaf_i2s_core eaf_i2s_core_t;

typedef struct {
    /* Prepare the device for the given rate and block size. */
    int (*configure)(eaf_i2s_core_t *core, const eaf_format_t *fmt, size_t frames);
    /* Called before the first write when enable_before_write is set. */
    int (*enable)(eaf_i2s_core_t *core);
    int (*write)(eaf_i2s_core_t *core, const void *data, size_t bytes, size_t *written);
    /* Called after the first successful write when enable_before_write is clear. */
    int (*start)(eaf_i2s_core_t *core);
    /* Optional immediate drop, used by stop after a drain. */
    int (*drop)(eaf_i2s_core_t *core);
    int (*drain)(eaf_i2s_core_t *core);
    /* Optional clock disable after a drain. */
    int (*disable)(eaf_i2s_core_t *core);
    /* Optional device teardown after stop. */
    int (*delete_channel)(eaf_i2s_core_t *core);
    int (*tune_ppm)(eaf_i2s_core_t *core, int32_t ppm);
    void *(*dma_alloc)(eaf_i2s_core_t *core, size_t bytes);
    void (*dma_free)(eaf_i2s_core_t *core, void *block);
    bool enable_before_write;
} eaf_i2s_backend_t;

struct eaf_i2s_core {
    const eaf_i2s_backend_t *backend;
    void *ctx;
    eaf_buffer_t buffer;
    void *dma;
    uint32_t frames;
    uint32_t submitted_blocks;
    bool configured, running, started, paused, held;
    void (*error_handler)(const char *operation, int error, uint32_t submitted_blocks);
};

extern const struct eaf_sink_ops eaf_i2s_core_sink_ops;

void eaf_i2s_core_set_error_handler(eaf_i2s_core_t *core,
                                    void (*handler)(const char *operation, int error,
                                                    uint32_t submitted_blocks));
int eaf_i2s_core_io_error(eaf_i2s_core_t *core, const char *operation, int error);
/* Audio-owner only, between blocks. Pause drains queued DMA, preserves PCM. */
int eaf_i2s_core_pause(eaf_i2s_core_t *core, bool paused);
