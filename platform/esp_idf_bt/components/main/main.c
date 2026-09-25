/* EAF ESP-IDF Bluetooth A2DP SBC sink.
 *
 * Board application scaffold. It owns Bluedroid/controller bring-up, the GAP
 * name and discoverability, the portable SBC decode worker and the output
 * owner that drains the jitter reservoir into the ESP-IDF I2S sink. Source
 * arbitration, volume/mute and pause are later T08 work. */
#include <eaf/eaf_bt.h>
#include <eaf/eaf_bt_decoder.h>
#include <eaf/eaf_bt_esp_idf.h>
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
#include <sdkconfig.h>

static const char *TAG = "eaf_bt";

static eaf_bt_ingress_t ingress;
static eaf_bt_decoder_t decoder;
static eaf_reservoir_t reservoir;
static int32_t reservoir_storage[CONFIG_EAF_BT_RESERVOIR_FRAMES * 2];
static eaf_sbc_oi_t sbc;
static eaf_sbc_decoder_t sbc_decoder;
static eaf_thread_t decode_thread;
static eaf_thread_t output_thread;

static void on_event(void *ctx, eaf_bt_esp_idf_event_t event) {
    (void)ctx;
    switch (event) {
    case EAF_BT_CONNECTED:
        ESP_LOGI(TAG, "source connected");
        break;
    case EAF_BT_DISCONNECTED:
        ESP_LOGI(TAG, "source disconnected");
        break;
    case EAF_BT_CONFIGURED:
        ESP_LOGI(TAG, "stream configured");
        break;
    case EAF_BT_STARTED:
        ESP_LOGI(TAG, "stream started");
        break;
    case EAF_BT_SUSPENDED:
        ESP_LOGI(TAG, "stream suspended");
        break;
    case EAF_BT_RELEASED:
        break;
    }
}

static void decode_entry(void *arg) {
    (void)arg;
    for (;;) {
        int rc = eaf_bt_decoder_step(&decoder);
        if (rc != EAF_OK)
            ESP_LOGW(TAG, "sbc decode step: %d", rc);
        vTaskDelay(1);
    }
}

static void output_entry(void *arg) {
    (void)arg;
    eaf_sink_t *sink = &eaf_esp_idf_i2s_sink;
    for (;;) {
        eaf_buffer_t *buffer = NULL;
        if (sink->ops->acquire_buf(sink, &buffer) != EAF_OK) {
            vTaskDelay(1);
            continue;
        }
        if (eaf_reservoir_pull(&reservoir, buffer) != EAF_OK) {
            vTaskDelay(1);
            continue;
        }
        int rc = sink->ops->commit_buf(sink, buffer);
        if (rc != EAF_OK)
            ESP_LOGW(TAG, "i2s commit: %d", rc);
    }
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

    eaf_bt_ingress_init(&ingress);
    if (eaf_sbc_oi_init(&sbc, &sbc_decoder) != EAF_OK)
        return EAF_IO;
    if (eaf_reservoir_init(&reservoir, reservoir_storage, CONFIG_EAF_BT_RESERVOIR_FRAMES, format,
                           CONFIG_EAF_BT_RESERVOIR_FRAMES / 2u) != EAF_OK)
        return EAF_IO;
    if (eaf_bt_decoder_init(&decoder, &ingress, &reservoir, &sbc_decoder) != EAF_OK)
        return EAF_IO;

    const eaf_thread_options_t decoder_options = {EAF_THREAD_DECODER, -1, false};
    const eaf_thread_options_t audio_options = {EAF_THREAD_AUDIO, -1, false};
    if (hal_thread_create_with_options(&decode_thread, decode_entry, NULL, &decoder_options) !=
        EAF_OK)
        return EAF_IO;
    if (hal_thread_create_with_options(&output_thread, output_entry, NULL, &audio_options) !=
        EAF_OK)
        return EAF_IO;
    return EAF_OK;
}

static int start_bluetooth(void) {
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&config));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(CONFIG_EAF_BT_DEVICE_NAME));
    if (eaf_bt_esp_idf_register(&ingress, CONFIG_EAF_BT_SAMPLE_RATE, on_event, NULL) != EAF_OK)
        return EAF_IO;
    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE));
    return EAF_OK;
}

void app_main(void) {
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
