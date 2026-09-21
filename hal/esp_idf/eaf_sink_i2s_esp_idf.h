#pragma once
#include <eaf/eaf_hal.h>

/* One statically allocated ESP-IDF TX sink. The board provides the I2S master
   clock and data-output pins; the pin description must outlive sink use.
   Supports stereo Q1.31, I2S master clocks only. */
typedef struct {
    int bclk_gpio;
    int ws_gpio;
    int dout_gpio;
} eaf_esp_idf_i2s_pins_t;

extern eaf_sink_t eaf_esp_idf_i2s_sink;

/* Pin description must outlive sink use. Call only before configure, while
   stopped. */
int eaf_esp_idf_i2s_bind(const eaf_esp_idf_i2s_pins_t *pins);

/* Optional error observer, called synchronously by the audio owner. Install only
   while stopped; callback must not re-enter the sink. NULL disables reporting. */
void eaf_esp_idf_i2s_set_error_handler(void (*handler)(const char *operation, int error,
                                                       uint32_t submitted_blocks));

/* Audio-owner only, between blocks. Pause drains queued DMA, preserves PCM. */
int eaf_esp_idf_i2s_pause(bool paused);
