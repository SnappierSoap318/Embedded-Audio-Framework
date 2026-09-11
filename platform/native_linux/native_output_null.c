#include "native_output.h"
int eaf_native_device_select(eaf_sink_t *sink, const char *device) {
    (void)sink;
    (void)device;
    return EAF_UNSUPPORTED;
}
int eaf_native_device_pause(bool paused) {
    (void)paused;
    return EAF_UNSUPPORTED;
}
uint32_t eaf_native_device_delay(void) {
    return 0;
}
