#include "credentials.h"
#include "board_config.h"

/* Private strings are never printed or passed to Kconfig. */
const char *board_wifi_ssid(void) {
    return EAF_WIFI_SSID;
}
const char *board_wifi_password(void) {
    return EAF_WIFI_PASSWORD;
}
