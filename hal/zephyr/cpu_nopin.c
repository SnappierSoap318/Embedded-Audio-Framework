#include "cpu_pin.h"

bool hal_zephyr_cpu_pin_available(void) {
    return false;
}

int hal_zephyr_cpu_pin(k_tid_t thread, int cpu) {
    (void)thread;
    (void)cpu;
    return -1;
}
