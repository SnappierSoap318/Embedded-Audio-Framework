#include "eaf_sink_i2s_esp_idf.h"
#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <sdkconfig.h>
#include <stdint.h>

#define EAF_I2S_DMA_DESC_NUM 4u
#define EAF_I2S_WRITE_TIMEOUT_MS 20u
/* I2S_STD_CLK_DEFAULT_CONFIG uses a 256x MCLK multiple. */
#define EAF_I2S_MCLK_MULTIPLE 256u

/* One fixed acquisition buffer; the ESP-IDF driver owns its own DMA storage, so
   only the caller-facing block needs to be handed out. No heap. */
static struct {
    const eaf_esp_idf_i2s_pins_t *pins;
    i2s_chan_handle_t tx;
    eaf_buffer_t buffer;
    int32_t storage[CONFIG_EAF_I2S_MAX_FRAMES * 2u];
    int32_t mclk_hz;
    uint32_t frames;
    uint32_t submitted_blocks;
    bool configured, running, started, paused, held, apll;
    void (*error_handler)(const char *operation, int error, uint32_t submitted_blocks);
} state;

void eaf_esp_idf_i2s_set_error_handler(void (*handler)(const char *operation, int error,
                                                       uint32_t submitted_blocks)) {
    state.error_handler = handler;
}

static int io_error(const char *operation, int error) {
    if (state.error_handler)
        state.error_handler(operation, error, state.submitted_blocks);
    return EAF_IO;
}

int eaf_esp_idf_i2s_bind(const eaf_esp_idf_i2s_pins_t *pins) {
    if (!pins || pins->bclk_gpio < 0 || pins->ws_gpio < 0 || pins->dout_gpio < 0)
        return EAF_INVALID;
    if (state.configured || state.running)
        return EAF_STATE;
    state.pins = pins;
    return EAF_OK;
}

/* Wait out the worst-case DMA residency, then stop the clock. The driver reports
   when a write has been queued, not when it reaches the amplifier, so this is a
   bounded software settle rather than a presentation timestamp. */
static int drain(void) {
    if (!state.started)
        return EAF_OK;
    uint64_t wait_us =
        (uint64_t)state.frames * EAF_I2S_DMA_DESC_NUM * 1000000u / state.buffer.format.sample_rate;
    if (hal_sleep_until_us(hal_monotonic_time_us() + wait_us + 1000u) != EAF_OK)
        return io_error("drain", (int)ESP_ERR_TIMEOUT);
    esp_err_t rc = i2s_channel_disable(state.tx);
    if (rc != ESP_OK)
        return io_error("disable", (int)rc);
    state.started = false;
    return EAF_OK;
}

static int init(eaf_sink_t *sink, const eaf_format_t *fmt, size_t frames) {
    (void)sink;
    if (state.configured || state.running)
        return EAF_STATE;
    if (!state.pins || !eaf_format_valid(fmt) || fmt->num_channels != 2 || fmt->channel_mask != 3 ||
        !frames || frames > CONFIG_EAF_I2S_MAX_FRAMES)
        return EAF_INVALID;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan.dma_desc_num = EAF_I2S_DMA_DESC_NUM;
    chan.dma_frame_num = (uint32_t)frames;
    chan.auto_clear = true;
    esp_err_t rc = i2s_new_channel(&chan, &state.tx, NULL);
    if (rc != ESP_OK)
        return io_error("new channel", (int)rc);
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(fmt->sample_rate),
        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED,
                     .bclk = (gpio_num_t)state.pins->bclk_gpio,
                     .ws = (gpio_num_t)state.pins->ws_gpio,
                     .dout = (gpio_num_t)state.pins->dout_gpio,
                     .din = I2S_GPIO_UNUSED},
    };
    state.apll = false;
#if SOC_I2S_SUPPORTS_APLL
    /* APLL gives i2s_channel_tune_rate the resolution adjust_ppm needs. */
    cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    rc = i2s_channel_init_std_mode(state.tx, &cfg);
    if (rc == ESP_OK)
        state.apll = true;
#endif
    if (!state.apll) {
        cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
        rc = i2s_channel_init_std_mode(state.tx, &cfg);
        if (rc != ESP_OK) {
            (void)i2s_del_channel(state.tx);
            state.tx = NULL;
            return io_error("init std", (int)rc);
        }
    }
    state.mclk_hz = (int32_t)(fmt->sample_rate * EAF_I2S_MCLK_MULTIPLE);
    state.frames = (uint32_t)frames;
    state.buffer = (eaf_buffer_t){.samples = state.storage,
                                  .frame_count = (uint32_t)frames,
                                  .capacity_frames = (uint32_t)frames,
                                  .capacity_samples = frames * 2u,
                                  .format = *fmt,
                                  .flags = 0};
    state.held = false;
    state.configured = true;
    return EAF_OK;
}

static int start(eaf_sink_t *sink) {
    (void)sink;
    if (!state.configured || state.running)
        return EAF_STATE;
    state.paused = false;
    state.submitted_blocks = 0;
    state.running = true;
    state.started = false;
    state.held = false;
    return EAF_OK;
}

static int acquire(eaf_sink_t *sink, eaf_buffer_t **buffer) {
    (void)sink;
    if (!state.running || state.paused || state.held || !buffer)
        return EAF_STATE;
    state.held = true;
    state.buffer.frame_count = state.frames;
    state.buffer.flags = 0;
    *buffer = &state.buffer;
    return EAF_OK;
}

static int commit(eaf_sink_t *sink, eaf_buffer_t *buffer) {
    (void)sink;
    if (!state.running || !state.held || buffer != &state.buffer)
        return EAF_STATE;
    if (!state.started) {
        esp_err_t erc = i2s_channel_enable(state.tx);
        if (erc != ESP_OK)
            return io_error("enable", (int)erc);
        state.started = true;
    }
    size_t bytes = (size_t)buffer->frame_count * 2u * sizeof(int32_t);
    size_t written = 0;
    esp_err_t wrc =
        i2s_channel_write(state.tx, state.storage, bytes, &written, EAF_I2S_WRITE_TIMEOUT_MS);
    if (wrc != ESP_OK || written != bytes)
        return io_error("write", wrc != ESP_OK ? (int)wrc : (int)ESP_ERR_TIMEOUT);
    state.held = false; /* Ownership transferred only on a complete write. */
    ++state.submitted_blocks;
    return (buffer->flags & EAF_FRAME_EOS) ? drain() : EAF_OK;
}

int eaf_esp_idf_i2s_pause(bool paused) {
    if (!state.running || state.held)
        return EAF_STATE;
    if (state.paused == paused)
        return EAF_OK;
    if (paused && state.started) {
        int rc = drain();
        if (rc)
            return rc;
    }
    state.paused = paused;
    return EAF_OK;
}

static int stop(eaf_sink_t *sink) {
    (void)sink;
    /* The uncommitted block is caller storage; there is nothing to reclaim. */
    state.held = false;
    if (state.started) {
        int rc = drain();
        if (rc)
            return rc;
    }
    state.running = false;
    state.started = false;
    state.paused = false;
    return EAF_OK;
}

static int deinit(eaf_sink_t *sink) {
    int rc = stop(sink);
    if (rc)
        return rc; /* Retain ownership/state if the driver cannot stop. */
    if (state.tx) {
        esp_err_t drc = i2s_del_channel(state.tx);
        if (drc != ESP_OK)
            return io_error("delete channel", (int)drc);
        state.tx = NULL;
    }
    state.configured = false;
    return EAF_OK;
}

static int adjust(eaf_sink_t *sink, int32_t ppm) {
    (void)sink;
    if (!state.configured || !state.apll || !state.tx)
        return EAF_UNSUPPORTED;
    int64_t mclk = state.mclk_hz;
    int64_t delta = mclk * (int64_t)ppm / 1000000;
    i2s_tuning_config_t tune = {
        .tune_mode = I2S_TUNING_MODE_SET,
        .tune_mclk_val = (int32_t)(mclk + delta),
        .max_delta_mclk = (int32_t)(mclk / 100),
        .min_delta_mclk = -(int32_t)(mclk / 100),
    };
    i2s_tuning_info_t info = {0};
    esp_err_t rc = i2s_channel_tune_rate(state.tx, &tune, &info);
    if (rc == ESP_OK)
        return EAF_OK;
    return rc == ESP_ERR_NOT_SUPPORTED ? EAF_UNSUPPORTED : EAF_IO;
}

static const struct eaf_sink_ops ops = {init, start, stop, acquire, commit, adjust, deinit};
eaf_sink_t eaf_esp_idf_i2s_sink = {&ops, &state};
