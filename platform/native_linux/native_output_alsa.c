#include "native_output.h"
#include <eaf/eaf_sink_alsa.h>
static eaf_alsa_sink_ctx_t alsa;
int eaf_native_device_select(eaf_sink_t *sink, const char *device) {
    if (alsa.pcm)
        return EAF_STATE;
    alsa.device = device;
    *sink = (eaf_sink_t){&eaf_alsa_sink_ops, &alsa};
    return EAF_OK;
}
int eaf_native_device_pause(bool paused) {
    return eaf_alsa_pause(&alsa, paused);
}
uint32_t eaf_native_device_delay(void) {
    return eaf_alsa_delay(&alsa);
}
