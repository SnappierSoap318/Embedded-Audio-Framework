#include "board_runtime.h"
#include "cpu_pin.h"

int board_cpu_pin(k_tid_t thread, int cpu) {
    if (cpu < 0)
        return EAF_OK;
    if (!hal_zephyr_cpu_pin_available())
        return EAF_UNSUPPORTED;
    return hal_zephyr_cpu_pin(thread, cpu) ? EAF_IO : EAF_OK;
}
