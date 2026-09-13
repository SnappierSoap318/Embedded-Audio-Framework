#include "cpu_pin.h"

bool hal_zephyr_cpu_pin_available(void) {
    return true;
}

int hal_zephyr_cpu_pin(k_tid_t thread, int cpu) {
    return k_thread_cpu_pin(thread, cpu);
}
