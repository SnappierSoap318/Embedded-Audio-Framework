#include "../common/sink_i2s_core.c"
#include <eaf/eaf_sink_i2s.h>
#include <errno.h>
#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#define SLOT_BYTES ROUND_UP((size_t)EAF_I2S_CORE_MAX_FRAMES * 2u * sizeof(int32_t), 32u)
/* Driver returns completed blocks to this fixed pool. No heap or growing pool. */
K_MEM_SLAB_DEFINE_STATIC(tx_blocks, SLOT_BYTES, 4, 32);

typedef struct {
    const char *name;
    const struct device *device;
} zephyr_i2s_ctx_t;

static int zephyr_configure(eaf_i2s_core_t *core, const eaf_format_t *fmt, size_t frames);
static int zephyr_write(eaf_i2s_core_t *core, const void *data, size_t bytes, size_t *written);
static int zephyr_start(eaf_i2s_core_t *core);
static int zephyr_drop(eaf_i2s_core_t *core);
static int zephyr_drain(eaf_i2s_core_t *core);
static void *zephyr_dma_alloc(eaf_i2s_core_t *core, size_t bytes);
static void zephyr_dma_free(eaf_i2s_core_t *core, void *block);

static const eaf_i2s_backend_t zephyr_backend = {.configure = zephyr_configure,
                                                 .enable = NULL,
                                                 .write = zephyr_write,
                                                 .start = zephyr_start,
                                                 .drop = zephyr_drop,
                                                 .drain = zephyr_drain,
                                                 .disable = NULL,
                                                 .delete_channel = NULL,
                                                 .tune_ppm = NULL,
                                                 .dma_alloc = zephyr_dma_alloc,
                                                 .dma_free = zephyr_dma_free,
                                                 .enable_before_write = false};

static zephyr_i2s_ctx_t zephyr_ctx;
static eaf_i2s_core_t zephyr_core = {.backend = &zephyr_backend, .ctx = &zephyr_ctx};

eaf_sink_t eaf_zephyr_i2s_sink = {&eaf_i2s_core_sink_ops, &zephyr_core};

void eaf_zephyr_i2s_set_error_handler(void (*handler)(const char *operation, int error,
                                                      uint32_t submitted_blocks)) {
    eaf_i2s_core_set_error_handler(&zephyr_core, handler);
}

int eaf_zephyr_i2s_bind(const char *name) {
    if (!name)
        return EAF_INVALID;
    if (zephyr_core.configured || zephyr_core.running)
        return EAF_STATE;
    zephyr_ctx.name = name;
    return EAF_OK;
}

int eaf_zephyr_i2s_pause(bool paused) {
    return eaf_i2s_core_pause(&zephyr_core, paused);
}

static int zephyr_configure(eaf_i2s_core_t *core, const eaf_format_t *fmt, size_t frames) {
    zephyr_i2s_ctx_t *c = core->ctx;
    if (!c->name)
        return EAF_INVALID;
    c->device = device_get_binding(c->name);
    if (!c->device || !device_is_ready(c->device))
        return eaf_i2s_core_io_error(core, "device ready", -ENODEV);
    struct i2s_config cfg = {.word_size = 32,
                             .channels = 2,
                             .format = I2S_FMT_DATA_FORMAT_I2S,
                             .options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
                             .frame_clk_freq = fmt->sample_rate,
                             .mem_slab = &tx_blocks,
                             .block_size = frames * 2u * sizeof(int32_t),
                             .timeout = 20};
    int rc = i2s_configure(c->device, I2S_DIR_TX, &cfg);
    if (rc)
        return eaf_i2s_core_io_error(core, "configure", rc);
    return EAF_OK;
}

static int zephyr_write(eaf_i2s_core_t *core, const void *data, size_t bytes, size_t *written) {
    (void)core;
    int cache_rc = sys_cache_data_flush_range((void *)data, bytes);
    if (cache_rc && cache_rc != -ENOTSUP)
        return cache_rc;
    int rc = i2s_write(zephyr_ctx.device, (void *)data, bytes);
    if (rc)
        return rc;
    *written = bytes;
    return EAF_OK;
}

static int zephyr_start(eaf_i2s_core_t *core) {
    (void)core;
    return i2s_trigger(zephyr_ctx.device, I2S_DIR_TX, I2S_TRIGGER_START);
}

static int zephyr_drop(eaf_i2s_core_t *core) {
    (void)core;
    return i2s_trigger(zephyr_ctx.device, I2S_DIR_TX, I2S_TRIGGER_DROP);
}

/* Reclaim every DMA slot after DRAIN. This waits for driver ownership release,
   not a measurement of the amplifier's presentation latency. */
static int zephyr_drain(eaf_i2s_core_t *core) {
    (void)core;
    int trigger_rc = i2s_trigger(zephyr_ctx.device, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
    if (trigger_rc)
        return trigger_rc;
    void *blocks[4];
    unsigned count = 0;
    int64_t deadline = k_uptime_get() + 1000;
    while (count < 4) {
        if (k_mem_slab_alloc(&tx_blocks, &blocks[count], K_TIMEOUT_ABS_MS(deadline)))
            break;
        ++count;
    }
    int rc = count == 4 ? EAF_OK : -ETIMEDOUT;
    while (count)
        k_mem_slab_free(&tx_blocks, blocks[--count]);
    return rc;
}

static void *zephyr_dma_alloc(eaf_i2s_core_t *core, size_t bytes) {
    (void)core;
    (void)bytes;
    void *block = NULL;
    if (k_mem_slab_alloc(&tx_blocks, &block, K_MSEC(20)))
        return NULL;
    return block;
}

static void zephyr_dma_free(eaf_i2s_core_t *core, void *block) {
    (void)core;
    if (block)
        k_mem_slab_free(&tx_blocks, block);
}
