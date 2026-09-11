#include "native_output.h"
#include <eaf/eaf_sink_null.h>
static bool device_selected, is_paused;
static uint64_t pause_started;
int eaf_native_output_select(eaf_sink_t *sink, const char *device) {
    if (!sink)
        return EAF_INVALID;
    if (!device)
        return device_selected ? EAF_STATE : EAF_OK;
    int rc = eaf_native_device_select(sink, device);
    if (!rc) {
        device_selected = true;
        is_paused = false;
    }
    return rc;
}
int eaf_native_output_pause(eaf_sink_t *sink, bool paused) {
    if (!sink || !sink->driver_data)
        return EAF_INVALID;
    if (device_selected)
        return eaf_native_device_pause(paused);
    eaf_null_sink_ctx_t *ctx = sink->driver_data;
    if (!ctx->running)
        return EAF_STATE;
    if (is_paused == paused)
        return EAF_OK;
    if (paused)
        pause_started = hal_monotonic_time_us();
    else {
        ctx->epoch_us += hal_monotonic_time_us() - pause_started;
    }
    is_paused = paused;
    return EAF_OK;
}
uint32_t eaf_native_output_delay(eaf_sink_t *sink) {
    (void)sink;
    return device_selected ? eaf_native_device_delay() : 0;
}
