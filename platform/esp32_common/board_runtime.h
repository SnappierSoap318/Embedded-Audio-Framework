#pragma once
#include <eaf/eaf_types.h>
#include <zephyr/kernel.h>

/* Small runtime shims whose implementation is chosen at build time, so app and
   diagnostics code does not need CONFIG feature branches. */

/* Pins a kernel thread to cpu; cpu < 0 is a no-op. Returns EAF_UNSUPPORTED when
   the kernel was built without CPU affinity. */
int board_cpu_pin(k_tid_t thread, int cpu);

/* Disables Wi-Fi modem sleep when the ESP32 Wi-Fi driver is present. */
void board_wifi_power_save_off(void);
