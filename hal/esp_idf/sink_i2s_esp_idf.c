#include "../common/sink_i2s_core.c"
#include "eaf_sink_i2s_esp_idf.h"
#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <sdkconfig.h>
#include <stdint.h>

#define EAF_I2S_DMA_DESC_NUM 4u
#define EAF_I2S_WRITE_TIMEOUT_MS 20u
/* I2S_STD_CLK_DEFAULT_CONFIG uses a 256x MCLK multiple. */
#define EAF_I2S_MCLK_MULTIPLE 256u

typedef struct {
    const eaf_esp_idf_i2s_pins_t *pins;
    i2s_chan_handle_t tx;
    int32_t storage[EAF_I2S_CORE_MAX_FRAMES * 2u];
    int32_t mclk_hz;
    bool apll;
} esp_i2s_ctx_t;

static int esp_configure(eaf_i2s_core_t *core, const eaf_format_t *fmt, size_t frames);
static int esp_enable(eaf_i2s_core_t *core);
static int esp_write(eaf_i2s_core_t *core, const void *data, size_t bytes, size_t *written);
static int esp_drain(eaf_i2s_core_t *core);
static int esp_disable(eaf_i2s_core_t *core);
static int esp_delete(eaf_i2s_core_t *core);
static int esp_tune_ppm(eaf_i2s_core_t *core, int32_t ppm);
static void *esp_dma_alloc(eaf_i2s_core_t *core, size_t bytes);
static void esp_dma_free(eaf_i2s_core_t *core, void *block);

static const eaf_i2s_backend_t esp_backend = {.configure = esp_configure,
                                              .enable = esp_enable,
                                              .write = esp_write,
                                              .start = NULL,
                                              .drop = NULL,
                                              .drain = esp_drain,
                                              .disable = esp_disable,
                                              .delete_channel = esp_delete,
                                              .tune_ppm = esp_tune_ppm,
                                              .dma_alloc = esp_dma_alloc,
                                              .dma_free = esp_dma_free,
                                              .enable_before_write = true};

static esp_i2s_ctx_t esp_ctx;
static eaf_i2s_core_t esp_core = {.backend = &esp_backend, .ctx = &esp_ctx};

eaf_sink_t eaf_esp_idf_i2s_sink = {&eaf_i2s_core_sink_ops, &esp_core};

void eaf_esp_idf_i2s_set_error_handler(void (*handler)(const char *operation, int error,
                                                       uint32_t submitted_blocks)) {
    eaf_i2s_core_set_error_handler(&esp_core, handler);
}

int eaf_esp_idf_i2s_bind(const eaf_esp_idf_i2s_pins_t *pins) {
    if (!pins || pins->bclk_gpio < 0 || pins->ws_gpio < 0 || pins->dout_gpio < 0)
        return EAF_INVALID;
    if (esp_core.configured || esp_core.running)
        return EAF_STATE;
    esp_ctx.pins = pins;
    return EAF_OK;
}

int eaf_esp_idf_i2s_pause(bool paused) {
    return eaf_i2s_core_pause(&esp_core, paused);
}

static int esp_configure(eaf_i2s_core_t *core, const eaf_format_t *fmt, size_t frames) {
    esp_i2s_ctx_t *c = core->ctx;
    if (!c->pins)
        return EAF_INVALID;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan.dma_desc_num = EAF_I2S_DMA_DESC_NUM;
    chan.dma_frame_num = (uint32_t)frames;
    chan.auto_clear = true;
    esp_err_t rc = i2s_new_channel(&chan, &c->tx, NULL);
    if (rc != ESP_OK)
        return eaf_i2s_core_io_error(core, "new channel", (int)rc);
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(fmt->sample_rate),
        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED,
                     .bclk = (gpio_num_t)c->pins->bclk_gpio,
                     .ws = (gpio_num_t)c->pins->ws_gpio,
                     .dout = (gpio_num_t)c->pins->dout_gpio,
                     .din = I2S_GPIO_UNUSED},
    };
    c->apll = false;
#if SOC_I2S_SUPPORTS_APLL
    /* APLL gives i2s_channel_tune_rate the resolution adjust_ppm needs. */
    cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    rc = i2s_channel_init_std_mode(c->tx, &cfg);
    if (rc == ESP_OK)
        c->apll = true;
#endif
    if (!c->apll) {
        cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
        rc = i2s_channel_init_std_mode(c->tx, &cfg);
        if (rc != ESP_OK) {
            (void)i2s_del_channel(c->tx);
            c->tx = NULL;
            return eaf_i2s_core_io_error(core, "init std", (int)rc);
        }
    }
    c->mclk_hz = (int32_t)(fmt->sample_rate * EAF_I2S_MCLK_MULTIPLE);
    return EAF_OK;
}

static int esp_enable(eaf_i2s_core_t *core) {
    esp_i2s_ctx_t *c = core->ctx;
    return (int)i2s_channel_enable(c->tx);
}

static int esp_write(eaf_i2s_core_t *core, const void *data, size_t bytes, size_t *written) {
    esp_i2s_ctx_t *c = core->ctx;
    esp_err_t rc = i2s_channel_write(c->tx, data, bytes, written, EAF_I2S_WRITE_TIMEOUT_MS);
    if (rc != ESP_OK)
        return (int)rc;
    return *written == bytes ? EAF_OK : (int)ESP_ERR_TIMEOUT;
}

/* Wait out the worst-case DMA residency, then let the core stop the clock. The
   driver reports when a write has been queued, not when it reaches the
   amplifier, so this is a bounded software settle rather than a presentation
   timestamp. */
static int esp_drain(eaf_i2s_core_t *core) {
    uint64_t wait_us =
        (uint64_t)core->frames * EAF_I2S_DMA_DESC_NUM * 1000000u / core->buffer.format.sample_rate;
    if (hal_sleep_until_us(hal_monotonic_time_us() + wait_us + 1000u) != EAF_OK)
        return (int)ESP_ERR_TIMEOUT;
    return EAF_OK;
}

static int esp_disable(eaf_i2s_core_t *core) {
    esp_i2s_ctx_t *c = core->ctx;
    return (int)i2s_channel_disable(c->tx);
}

static int esp_delete(eaf_i2s_core_t *core) {
    esp_i2s_ctx_t *c = core->ctx;
    if (!c->tx)
        return EAF_OK;
    esp_err_t rc = i2s_del_channel(c->tx);
    if (rc != ESP_OK)
        return (int)rc;
    c->tx = NULL;
    return EAF_OK;
}

static int esp_tune_ppm(eaf_i2s_core_t *core, int32_t ppm) {
    esp_i2s_ctx_t *c = core->ctx;
    if (!core->configured || !c->apll || !c->tx)
        return EAF_UNSUPPORTED;
    int64_t mclk = c->mclk_hz;
    int64_t delta = mclk * (int64_t)ppm / 1000000;
    i2s_tuning_config_t tune = {
        .tune_mode = I2S_TUNING_MODE_SET,
        .tune_mclk_val = (int32_t)(mclk + delta),
        .max_delta_mclk = (int32_t)(mclk / 100),
        .min_delta_mclk = -(int32_t)(mclk / 100),
    };
    i2s_tuning_info_t info = {0};
    esp_err_t rc = i2s_channel_tune_rate(c->tx, &tune, &info);
    if (rc == ESP_OK)
        return EAF_OK;
    return rc == ESP_ERR_NOT_SUPPORTED ? EAF_UNSUPPORTED : EAF_IO;
}

static void *esp_dma_alloc(eaf_i2s_core_t *core, size_t bytes) {
    (void)bytes;
    esp_i2s_ctx_t *c = core->ctx;
    return c->storage;
}

static void esp_dma_free(eaf_i2s_core_t *core, void *block) {
    (void)core;
    (void)block;
}
