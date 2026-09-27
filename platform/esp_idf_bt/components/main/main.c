/* EAF ESP-IDF Bluetooth A2DP SBC sink.
 *
 * Board application scaffold. It owns Bluedroid/controller bring-up, the GAP
 * name and discoverability, the portable SBC decode worker and the output
 * owner that drains the jitter reservoir into the ESP-IDF I2S sink. Source
 * arbitration with other sources remains separate work. */
#include "amp.h"
#include "speaker.h"
#include <eaf/eaf_bt.h>
#include <eaf/eaf_bt_decoder.h>
#include <eaf/eaf_bt_esp_idf.h>
#include <eaf/eaf_bt_volume.h>
#include <eaf/eaf_hal.h>
#include <eaf/eaf_sbc_oi.h>
#include <eaf_sink_i2s_esp_idf.h>
#include <esp_bt.h>
#include <esp_bt_device.h>
#include <esp_bt_main.h>
#include <esp_err.h>
#include <esp_gap_bt_api.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <sdkconfig.h>
#include <string.h>

static const char *TAG = "eaf_bt";

static eaf_bt_ingress_t ingress;
static eaf_bt_decoder_t decoder;
static eaf_reservoir_t reservoir;
static int32_t reservoir_storage[CONFIG_EAF_BT_RESERVOIR_FRAMES * 2];
static eaf_sbc_oi_t sbc;
static eaf_sbc_decoder_t sbc_decoder;
static eaf_thread_t output_thread;

static void on_event(void *ctx, eaf_bt_esp_idf_event_t event) {
    (void)ctx;
    (void)event;
    /* Workers poll the binding's lossless generation/status snapshot. */
}

static void output_entry(void *arg) {
    (void)arg;
    eaf_sink_t *sink = &eaf_esp_idf_i2s_sink;
    eaf_bt_esp_idf_status_t stream = {0};
    eaf_bt_volume_t gain = {0};
    unsigned errors = 0, sink_failures = 0;
    /* Restarting the sink and dropping queued audio is the recovery for both a
     * stream boundary and a transient I2S write failure; bounded so a hardware
     * fault still stops instead of looping. */
    for (;;) {
        /* One worker owns BOTH reservoir cursors and decoder state. No reset
         * races the output reader. sync purges ingress with its producer gated. */
        if (eaf_bt_esp_idf_sync(&stream)) {
            if (sink->ops->stop(sink) != EAF_OK || sink->ops->start(sink) != EAF_OK)
                break;
            eaf_reservoir_reset(&reservoir);
            if (eaf_bt_decoder_reset(&decoder) != EAF_OK)
                break;
            gain = (eaf_bt_volume_t){0};
        }
        if (stream.streaming) {
            /* Bounded catch-up; each step decodes at most 128 frames. */
            for (unsigned i = 0; i < 16 && !eaf_reservoir_backpressure(&reservoir); ++i) {
                int rc = eaf_bt_decoder_step(&decoder);
                if (rc != EAF_OK && (++errors & 127u) == 1u)
                    ESP_LOGW(TAG, "SBC decode error=%d total=%u", rc, errors);
            }
        }
        eaf_buffer_t *buffer = NULL;
        if (sink->ops->acquire_buf(sink, &buffer) != EAF_OK) {
            if (++sink_failures > 8)
                break;
            vTaskDelay(1);
            continue;
        }
        if (eaf_reservoir_pull(&reservoir, buffer) != EAF_OK)
            break;
        eaf_bt_esp_idf_status_t latest;
        eaf_bt_esp_idf_status(&latest);
        if (!latest.streaming || latest.generation != stream.generation)
            memset(buffer->samples, 0, buffer->frame_count * 2u * sizeof(int32_t));
        unsigned level = speaker_audio_level();
        eaf_bt_volume_apply(&gain, buffer->samples, buffer->frame_count, (uint8_t)(level & 127u),
                            (level & 128u) != 0);
        if (sink->ops->commit_buf(sink, buffer) != EAF_OK) {
            /* A failed write leaves the sink holding the block, so drain and
             * restart before retrying; a persistent fault ends the worker. */
            if (sink->ops->stop(sink) != EAF_OK || sink->ops->start(sink) != EAF_OK)
                break;
            eaf_reservoir_reset(&reservoir);
            if (eaf_bt_decoder_reset(&decoder) != EAF_OK)
                break;
            gain = (eaf_bt_volume_t){0};
            if (++sink_failures > 8)
                break;
            vTaskDelay(1);
            continue;
        }
        sink_failures = 0;
        /* The blocking I2S write paces this worker even while output is silent. */
    }
    ESP_LOGE(TAG, "audio owner stopped after sink/decoder failure");
    (void)sink->ops->stop(sink);
}

static int start_audio(void) {
    static const eaf_esp_idf_i2s_pins_t pins = {
        .bclk_gpio = CONFIG_EAF_I2S_BCLK_GPIO,
        .ws_gpio = CONFIG_EAF_I2S_WS_GPIO,
        .dout_gpio = CONFIG_EAF_I2S_DOUT_GPIO,
    };
    const eaf_format_t format = {.sample_rate = CONFIG_EAF_BT_SAMPLE_RATE,
                                 .num_channels = 2,
                                 .channel_mask = EAF_CH_FRONT_LEFT | EAF_CH_FRONT_RIGHT};
    eaf_sink_t *sink = &eaf_esp_idf_i2s_sink;
    if (eaf_esp_idf_i2s_bind(&pins) != EAF_OK)
        return EAF_INVALID;
    if (sink->ops->init(sink, &format, CONFIG_EAF_BT_BLOCK_FRAMES) != EAF_OK)
        return EAF_IO;
    if (sink->ops->start(sink) != EAF_OK)
        return EAF_IO;

    /* The IDF sink enables clocks on its first commit, not on start().
     * The amplifier needs running clocks before entering Play. */
    eaf_buffer_t *silence = NULL;
    if (sink->ops->acquire_buf(sink, &silence) != EAF_OK)
        return EAF_IO;
    memset(silence->samples, 0, silence->capacity_samples * sizeof(int32_t));
    if (sink->ops->commit_buf(sink, silence) != EAF_OK)
        return EAF_IO;
    if (board_amp_start() != EAF_OK)
        return EAF_IO;

    eaf_bt_ingress_init(&ingress);
    if (eaf_sbc_oi_init(&sbc, &sbc_decoder) != EAF_OK)
        return EAF_IO;
    if (eaf_reservoir_init(&reservoir, reservoir_storage, CONFIG_EAF_BT_RESERVOIR_FRAMES, format,
                           CONFIG_EAF_BT_RESERVOIR_FRAMES / 2u) != EAF_OK)
        return EAF_IO;
    if (eaf_bt_decoder_init(&decoder, &ingress, &reservoir, &sbc_decoder) != EAF_OK)
        return EAF_IO;

    const eaf_thread_options_t audio_options = {EAF_THREAD_AUDIO, -1, false};
    if (hal_thread_create_with_options(&output_thread, output_entry, NULL, &audio_options) !=
        EAF_OK)
        return EAF_IO;
    return EAF_OK;
}

static int start_bluetooth(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    /* Initialization and enable must use the same mode, including when an
     * existing sdkconfig still selects the dual-mode controller default. */
    config.mode = ESP_BT_MODE_CLASSIC_BT;
    ESP_ERROR_CHECK(esp_bt_controller_init(&config));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    if (speaker_start() != EAF_OK)
        return EAF_IO;
    if (eaf_bt_esp_idf_register(&ingress, CONFIG_EAF_BT_SAMPLE_RATE, on_event, NULL) != EAF_OK)
        return EAF_IO;
    return EAF_OK;
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting EAF Bluetooth audio");
    if (start_audio() != EAF_OK) {
        ESP_LOGE(TAG, "EAF Bluetooth audio start failed");
        return;
    }
    if (start_bluetooth() != EAF_OK) {
        ESP_LOGE(TAG, "EAF Bluetooth stack start failed");
        return;
    }
    ESP_LOGI(TAG, "EAF Bluetooth speaker ready as \"%s\"", CONFIG_EAF_BT_DEVICE_NAME);
}
