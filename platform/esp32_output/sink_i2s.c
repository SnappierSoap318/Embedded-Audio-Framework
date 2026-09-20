#include "../esp32_common/diagnostics.h"
#include "board_output.h"
#include <eaf/eaf_sink_i2s.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

static void sink_error(const char *operation, int error, uint32_t submitted_blocks) {
    board_log_memory("I2S %s failed: driver rc=%d submitted=%u", operation, error,
                     (unsigned)submitted_blocks);
}
eaf_sink_t *board_sink(void) {
    return &eaf_zephyr_i2s_sink;
}
int board_sink_init(void) {
    eaf_zephyr_i2s_set_error_handler(sink_error);
    return eaf_zephyr_i2s_bind(DEVICE_DT_NAME(DT_NODELABEL(i2s0)));
}
int board_sink_pause(bool paused) {
    return eaf_zephyr_i2s_pause(paused);
}
