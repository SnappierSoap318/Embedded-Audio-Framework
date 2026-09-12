#include "output.h"
#include <eaf/eaf_sink_i2s.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
eaf_sink_t *board_sink(void) {
    return &eaf_zephyr_i2s_sink;
}
int board_sink_init(void) {
    return eaf_zephyr_i2s_bind(DEVICE_DT_NAME(DT_NODELABEL(i2s0)));
}
int board_sink_pause(bool paused) {
    return eaf_zephyr_i2s_pause(paused);
}
