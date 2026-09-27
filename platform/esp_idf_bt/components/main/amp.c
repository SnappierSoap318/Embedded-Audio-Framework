#include "amp.h"
#include "tas5805m.h"
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <eaf/eaf_types.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <string.h>

static const char *TAG = "eaf_amp";
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t amp;

static int amp_write(uint8_t reg, const uint8_t *data, size_t length, void *ctx) {
    (void)ctx;
    uint8_t buffer[8];
    if (length + 1u > sizeof(buffer))
        return -1;
    buffer[0] = reg;
    memcpy(buffer + 1u, data, length);
    return i2c_master_transmit(amp, buffer, length + 1u, 100) == ESP_OK ? 0 : -1;
}

static bool amp_fault_asserted(void *ctx) {
    (void)ctx;
    return gpio_get_level(CONFIG_EAF_AMP_FAULT_GPIO) == 0;
}

static const tas5805m_io_t io = {amp_write, amp_fault_asserted, NULL};

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

    if (tas5805m_bringup(&io) != EAF_OK)
        goto fail;
    ESP_LOGI(TAG, "TAS5805M playing at 0x%02x, fault=%d", CONFIG_EAF_AMP_I2C_ADDRESS,
             (int)tas5805m_fault(&io));
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
