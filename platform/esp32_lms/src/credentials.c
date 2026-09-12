#include "credentials.h"
#include "output.h"
/* Separate TU keeps the no-credentials build's main path representative for
   link/memory checks. Private strings are never printed or passed to Kconfig. */
const char *board_wifi_ssid(void) {
    return EAF_WIFI_SSID;
}
const char *board_wifi_password(void) {
    return EAF_WIFI_PASSWORD;
}
