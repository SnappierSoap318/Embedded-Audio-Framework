#pragma once
#include <stdbool.h>
#include <stddef.h>

/* OTA upload handling for the diagnostics HTTP server. The implementation is
   selected at build time: ota_mcuboot.c when CONFIG_BOOTLOADER_MCUBOOT is set,
   otherwise ota_stub.c. */

/* True when the running image supports OTA (MCUboot). */
bool ota_available(void);

/* Handles a POST /ota request. `request` holds the bytes already read, NUL
   terminated, including the header terminator and any body bytes. Returns true
   if the request was handled (the caller must close the socket). */
bool ota_handle_request(int fd, const char *request, size_t used);

/* Set after an image is written and the upgrade requested. The application must
   reboot from a clean context (sys_reboot cannot run while the HTTP handler
   holds locks). */
bool ota_reboot_requested(void);
