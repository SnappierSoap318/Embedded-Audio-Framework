#include "tas5805m.h"
#include "diagnostics.h"
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
static bool amp_present;

static const struct device *amp_gpio_dev(int gpio) {
    return gpio < GPIO_SPLIT ? gpio_low : gpio_high;
}

static gpio_pin_t amp_gpio_pin(int gpio) {
    return (gpio_pin_t)(gpio < GPIO_SPLIT ? gpio : gpio - GPIO_SPLIT);
}

static int reg_write(uint8_t reg, const uint8_t *data, size_t length) {
    uint8_t buffer[8];
    if (length + 1u > sizeof(buffer))
        return -1;
    buffer[0] = reg;
    memcpy(buffer + 1u, data, length);
    return i2c_write(i2c_bus, buffer, length + 1u, amp_address);
}

static int reg_write_u8(uint8_t reg, uint8_t value) {
    return reg_write(reg, &value, 1u);
}

static int reg_probe(uint8_t address) {
    uint8_t reg = 0x00u;
    uint8_t value = 0x00u;
    return i2c_write_read(i2c_bus, address, &reg, 1u, &value, 1u);
}

static void select_book_page(uint8_t book, uint8_t page) {
    (void)reg_write_u8(0x00u, page);
    (void)reg_write_u8(0x7Fu, book);
    (void)reg_write_u8(0x00u, page);
}

int tas5805m_bringup(void) {
    const struct device *pwdn_dev = amp_gpio_dev(CONFIG_EAF_AMP_PWDN_GPIO);
    const struct device *fault_dev = amp_gpio_dev(CONFIG_EAF_AMP_FAULT_GPIO);
    gpio_pin_t pwdn_pin = amp_gpio_pin(CONFIG_EAF_AMP_PWDN_GPIO);
    gpio_pin_t fault_pin = amp_gpio_pin(CONFIG_EAF_AMP_FAULT_GPIO);
    if (!device_is_ready(i2c_bus) || !device_is_ready(pwdn_dev) || !device_is_ready(fault_dev)) {
        board_log("TAS5805M: I2C or GPIO device not ready");
        return EAF_IO;
    }
    if (gpio_pin_configure(fault_dev, fault_pin, GPIO_INPUT) ||
        gpio_pin_configure(pwdn_dev, pwdn_pin, GPIO_OUTPUT_ACTIVE)) {
        board_log("TAS5805M: GPIO configure failed");
        return EAF_IO;
    }
    k_sleep(K_MSEC(10));

    amp_address = 0u;
    for (uint8_t address = 0x08u; address <= 0x77u; ++address) {
        if (reg_probe(address) == 0) {
            board_log("TAS5805M: I2C device at 0x%02x", (unsigned)address);
            if (address >= TAS5805M_ADDR_FIRST && address <= TAS5805M_ADDR_LAST && !amp_address)
                amp_address = address;
        }
    }
    if (!amp_address) {
        board_log("TAS5805M: no amp on 0x%02x-0x%02x (PDN=%d fault=%d)",
                  (unsigned)TAS5805M_ADDR_FIRST, (unsigned)TAS5805M_ADDR_LAST,
                  (int)gpio_pin_get(pwdn_dev, pwdn_pin), (int)gpio_pin_get(fault_dev, fault_pin));
        return EAF_IO;
    }
    amp_present = true;
    board_log("TAS5805M: found at 0x%02x (fault=%d)", (unsigned)amp_address,
              (int)gpio_pin_get(fault_dev, fault_pin));

    /* Basic play configuration (BD modulation, 0 dB, HiZ -> Play). */
    select_book_page(0x00u, 0x00u);
    (void)reg_write_u8(0x02u, 0x00u); /* DAMP_MOD: BD modulation */
    (void)reg_write_u8(0x53u, 0x60u); /* Class-D loop bandwidth 175 kHz */
    (void)reg_write_u8(0x61u, 0x0Bu); /* ADR/FAULT pin as FAULT */
    select_book_page(0x8Cu, 0x2Au);   /* DSP host memory volume page */
    static const uint8_t volume_0db[4] = {0x00u, 0x80u, 0x00u, 0x00u};
    (void)reg_write(0x24u, volume_0db, sizeof(volume_0db));
    select_book_page(0x00u, 0x00u);
    (void)reg_write_u8(0x03u, 0x02u); /* HiZ */
    k_sleep(K_MSEC(5));
    return tas5805m_play();
}

int tas5805m_play(void) {
    if (!amp_present)
        return EAF_UNSUPPORTED;
    select_book_page(0x00u, 0x00u);
    (void)reg_write_u8(0x03u, 0x03u); /* Play */
    return EAF_OK;
}

bool tas5805m_present(void) {
    return amp_present;
}

bool tas5805m_fault(void) {
    const struct device *fault_dev = amp_gpio_dev(CONFIG_EAF_AMP_FAULT_GPIO);
    if (!amp_present || !device_is_ready(fault_dev))
        return false;
    /* FAULT is open-drain, active low (external pull-up on this carrier). */
    return gpio_pin_get(fault_dev, amp_gpio_pin(CONFIG_EAF_AMP_FAULT_GPIO)) == 0;
}
