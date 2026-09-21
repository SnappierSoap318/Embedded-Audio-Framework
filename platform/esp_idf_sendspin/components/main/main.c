#include <eaf/eaf_hal.h>
#include <eaf_sink_i2s_esp_idf.h>
#include <esp_log.h>
#include <math.h>
#include <sdkconfig.h>
#include <stdint.h>

static const char *TAG = "eaf_smoke";

static eaf_atomic_u32_t worker_ran;

static void worker_entry(void *arg) {
    (void)arg;
    hal_atomic_set(&worker_ran, 1u);
}

static int smoke_hal(void) {
    eaf_sem_t sem = {0};
    if (hal_sem_init(&sem) != EAF_OK)
        return EAF_IO;
    if (hal_sem_take(&sem, 1) != EAF_TIMEOUT)
        return EAF_IO;
    hal_sem_give(&sem);
    if (hal_sem_take(&sem, 100) != EAF_OK)
        return EAF_IO;
    hal_sem_deinit(&sem);

    static eaf_thread_t thread;
    const eaf_thread_options_t options = {EAF_THREAD_AUDIO, -1, false};
    if (hal_thread_create_with_options(&thread, worker_entry, NULL, &options) != EAF_OK)
        return EAF_IO;
    if (hal_thread_join(&thread) != EAF_OK)
        return EAF_IO;
    if (hal_atomic_get(&worker_ran) != 1u)
        return EAF_IO;
    ESP_LOGI(TAG, "HAL semaphore, thread and monotonic clock ok");
    return EAF_OK;
}

static void fill_tone(int32_t *out, uint32_t frames, float *phase, float step) {
    for (uint32_t i = 0; i < frames; ++i) {
        int32_t sample = (int32_t)(sinf(*phase) * (float)INT32_MAX * 0.25f);
        out[2u * i] = sample;
        out[2u * i + 1u] = sample;
        *phase += step;
        if (*phase >= 2.0f * (float)M_PI)
            *phase -= 2.0f * (float)M_PI;
    }
}

static int smoke_i2s(void) {
    static const eaf_esp_idf_i2s_pins_t pins = {
        .bclk_gpio = CONFIG_EAF_I2S_BCLK_GPIO,
        .ws_gpio = CONFIG_EAF_I2S_WS_GPIO,
        .dout_gpio = CONFIG_EAF_I2S_DOUT_GPIO,
    };
    eaf_sink_t *sink = &eaf_esp_idf_i2s_sink;
    if (eaf_esp_idf_i2s_bind(&pins) != EAF_OK)
        return EAF_IO;
    const eaf_format_t format = {.sample_rate = CONFIG_EAF_SMOKE_SAMPLE_RATE,
                                 .num_channels = 2,
                                 .channel_mask = EAF_CH_FRONT_LEFT | EAF_CH_FRONT_RIGHT};
    const uint32_t frames = 128u;
    if (sink->ops->init(sink, &format, frames) != EAF_OK)
        return EAF_IO;
    if (sink->ops->start(sink) != EAF_OK)
        return EAF_IO;
    float phase = 0.0f;
    float step = 2.0f * (float)M_PI * 440.0f / (float)format.sample_rate;
    uint64_t block_us = (uint64_t)frames * 1000000u / format.sample_rate;
    uint64_t deadline = hal_monotonic_time_us();
    for (uint32_t block = 0; block < CONFIG_EAF_SMOKE_BLOCKS; ++block) {
        eaf_buffer_t *buffer = NULL;
        if (sink->ops->acquire_buf(sink, &buffer) != EAF_OK)
            return EAF_IO;
        fill_tone(buffer->samples, buffer->frame_count, &phase, step);
        if (block + 1u == CONFIG_EAF_SMOKE_BLOCKS)
            buffer->flags |= EAF_FRAME_EOS;
        if (sink->ops->commit_buf(sink, buffer) != EAF_OK)
            return EAF_IO;
        deadline += block_us;
        (void)hal_sleep_until_us(deadline);
    }
    if (sink->ops->deinit(sink) != EAF_OK)
        return EAF_IO;
    ESP_LOGI(TAG, "I2S sink played %u frames at %u Hz", (unsigned)CONFIG_EAF_SMOKE_BLOCKS * frames,
             (unsigned)format.sample_rate);
    return EAF_OK;
}

void app_main(void) {
    ESP_LOGI(TAG, "EAF ESP-IDF smoke start");
    if (smoke_hal() != EAF_OK || smoke_i2s() != EAF_OK) {
        ESP_LOGE(TAG, "EAF ESP-IDF smoke failed");
        return;
    }
    ESP_LOGI(TAG, "EAF ESP-IDF smoke complete");
}
