#include "amp.h"
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <eaf/eaf_types.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

static const char *TAG = "eaf_amp";
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t amp;

static esp_err_t write_reg(uint8_t reg, uint8_t value) {
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(amp, data, sizeof(data), 100);
}

int board_amp_start(void) {
    esp_err_t err = gpio_set_level(CONFIG_EAF_AMP_PWDN_GPIO, 0);
    if (err == ESP_OK)
        err = gpio_set_direction(CONFIG_EAF_AMP_PWDN_GPIO, GPIO_MODE_OUTPUT);
    if (err == ESP_OK)
        err = gpio_set_direction(CONFIG_EAF_AMP_FAULT_GPIO, GPIO_MODE_INPUT);
    if (err != ESP_OK)
        goto fail;
    vTaskDelay(pdMS_TO_TICKS(10));
    err = gpio_set_level(CONFIG_EAF_AMP_PWDN_GPIO, 1);
    if (err != ESP_OK)
        goto fail;
    vTaskDelay(pdMS_TO_TICKS(10));

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_EAF_AMP_SDA_GPIO,
        .scl_io_num = CONFIG_EAF_AMP_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    err = i2c_new_master_bus(&bus_config, &bus);
    if (err != ESP_OK)
        goto fail;
    const i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_EAF_AMP_I2C_ADDRESS,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(bus, &device, &amp);
    if (err != ESP_OK)
        goto fail;

    /* Book/page selection must return to page zero before changing books.
     * Match the carrier's Zephyr setup: BD modulation, 32-bit standard I2S,
     * 175 kHz loop bandwidth and the ADR/FAULT pin configured as FAULT. */
    static const uint8_t setup[][2] = {
        {0x00, 0x00}, {0x7f, 0x00}, {0x03, 0x02}, {0x02, 0x00},
        {0x33, 0x03}, {0x53, 0x60}, {0x61, 0x0b},
    };
    for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]); ++i) {
        err = write_reg(setup[i][0], setup[i][1]);
        if (err != ESP_OK)
            goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
    err = write_reg(0x03, 0x03); /* Play, unmuted. */
    if (err != ESP_OK)
        goto fail;
    ESP_LOGI(TAG, "TAS5805M playing at 0x%02x, fault=%d", CONFIG_EAF_AMP_I2C_ADDRESS,
             gpio_get_level(CONFIG_EAF_AMP_FAULT_GPIO) == 0);
    return EAF_OK;

fail:
    ESP_LOGE(TAG, "TAS5805M initialization failed: %s", esp_err_to_name(err));
    (void)gpio_set_level(CONFIG_EAF_AMP_PWDN_GPIO, 0);
    if (amp) {
        (void)i2c_master_bus_rm_device(amp);
        amp = NULL;
    }
    if (bus) {
        (void)i2c_del_master_bus(bus);
        bus = NULL;
    }
    return EAF_IO;
}
