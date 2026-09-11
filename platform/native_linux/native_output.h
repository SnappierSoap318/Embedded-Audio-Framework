#pragma once
#include <eaf/eaf_hal.h>
/* One output per native executable. Select before configuration; pause/delay
   belong exclusively to the audio owner. NULL device preserves the null sink. */
int eaf_native_output_select(eaf_sink_t *sink, const char *device);
int eaf_native_output_pause(eaf_sink_t *sink, bool paused);
uint32_t eaf_native_output_delay(eaf_sink_t *sink);
/* Build-selected backend, used by native_output.c only. */
int eaf_native_device_select(eaf_sink_t *sink, const char *device);
int eaf_native_device_pause(bool paused);
uint32_t eaf_native_device_delay(void);
