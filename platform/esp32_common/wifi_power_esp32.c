#include "board_runtime.h"
#include "diagnostics.h"
#include <esp_wifi.h>

void board_wifi_power_save_off(void) {
    if (IS_ENABLED(CONFIG_EAF_BOARD_WIFI_PS_NONE))
        board_log("Wi-Fi power save off rc=%d", (int)esp_wifi_set_ps(WIFI_PS_NONE));
}
