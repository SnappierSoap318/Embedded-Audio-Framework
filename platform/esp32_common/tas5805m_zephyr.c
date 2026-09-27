#include "diagnostics.h"
#include "tas5805m.h"
#include <stdint.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

/* TAS5805M 7-bit addresses (ADR pin strap): 0x2C..0x2F. */
#define TAS5805M_ADDR_FIRST 0x2Cu
#define TAS5805M_ADDR_LAST 0x2Fu

/* ESP32 GPIO32-39 live on the second controller. */
#define GPIO_SPLIT 32

static const struct device *const i2c_bus = DEVICE_DT_GET(DT_ALIAS(i2c_0));
static const struct device *const gpio_low = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *const gpio_high = DEVICE_DT_GET(DT_NODELABEL(gpio1));
static uint8_t amp_address;

static const struct device *amp_gpio_dev(int gpio) {
    return gpio < GPIO_SPLIT ? gpio_low : gpio_high;
}

static gpio_pin_t amp_gpio_pin(int gpio) {
    return (gpio_pin_t)(gpio < GPIO_SPLIT ? gpio : gpio - GPIO_SPLIT);
}

static int zephyr_write(uint8_t reg, const uint8_t *data, size_t length, void *ctx) {
    (void)ctx;
    uint8_t buffer[8];
    if (length + 1u > sizeof(buffer))
        return -1;
    buffer[0] = reg;
    memcpy(buffer + 1u, data, length);
    return i2c_write(i2c_bus, buffer, length + 1u, amp_address);
}

static bool zephyr_fault(void *ctx) {
    (void)ctx;
    const struct device *fault_dev = amp_gpio_dev(CONFIG_EAF_AMP_FAULT_GPIO);
    if (!device_is_ready(fault_dev))
        return false;
    /* FAULT is open-drain, active low (external pull-up on this carrier). */
    return gpio_pin_get(fault_dev, amp_gpio_pin(CONFIG_EAF_AMP_FAULT_GPIO)) == 0;
}

static const tas5805m_io_t board_io = {zephyr_write, zephyr_fault, NULL};

const tas5805m_io_t *tas5805m_board_io(void) {
    const struct device *pwdn_dev = amp_gpio_dev(CONFIG_EAF_AMP_PWDN_GPIO);
    const struct device *fault_dev = amp_gpio_dev(CONFIG_EAF_AMP_FAULT_GPIO);
    gpio_pin_t pwdn_pin = amp_gpio_pin(CONFIG_EAF_AMP_PWDN_GPIO);
    gpio_pin_t fault_pin = amp_gpio_pin(CONFIG_EAF_AMP_FAULT_GPIO);
    if (!device_is_ready(i2c_bus) || !device_is_ready(pwdn_dev) || !device_is_ready(fault_dev)) {
        board_log("TAS5805M: I2C or GPIO device not ready");
        return NULL;
    }
    if (gpio_pin_configure(fault_dev, fault_pin, GPIO_INPUT) ||
        gpio_pin_configure(pwdn_dev, pwdn_pin, GPIO_OUTPUT_ACTIVE)) {
        board_log("TAS5805M: GPIO configure failed");
        return NULL;
    }
    k_sleep(K_MSEC(10));

    amp_address = 0u;
    for (uint8_t address = 0x08u; address <= 0x77u; ++address) {
        uint8_t reg = 0x00u;
        uint8_t value = 0x00u;
        if (i2c_write_read(i2c_bus, address, &reg, 1u, &value, 1u) == 0) {
            board_log("TAS5805M: I2C device at 0x%02x", (unsigned)address);
            if (address >= TAS5805M_ADDR_FIRST && address <= TAS5805M_ADDR_LAST && !amp_address)
                amp_address = address;
        }
    }
    if (!amp_address) {
        board_log("TAS5805M: no amp on 0x%02x-0x%02x (PDN=%d fault=%d)",
                  (unsigned)TAS5805M_ADDR_FIRST, (unsigned)TAS5805M_ADDR_LAST,
                  (int)gpio_pin_get(pwdn_dev, pwdn_pin), (int)gpio_pin_get(fault_dev, fault_pin));
        return NULL;
    }
    board_log("TAS5805M: found at 0x%02x (fault=%d)", (unsigned)amp_address,
              (int)gpio_pin_get(fault_dev, fault_pin));
    return &board_io;
}
