#pragma once
#include <eaf/eaf_hal.h>
/* One statically allocated Zephyr TX sink. Board must configure pins, codec and
   DMA-accessible placement; supports stereo Q1.31, I2S master clocks only. */
extern eaf_sink_t eaf_zephyr_i2s_sink;
/* Device name must outlive sink use. Call only before configure, while stopped. */
int eaf_zephyr_i2s_bind(const char *device_name);
