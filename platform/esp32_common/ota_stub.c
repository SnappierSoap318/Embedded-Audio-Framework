#include "ota.h"

bool ota_available(void) {
    return false;
}

bool ota_handle_request(int fd, const char *request, size_t used) {
    (void)fd;
    (void)request;
    (void)used;
    return false;
}
