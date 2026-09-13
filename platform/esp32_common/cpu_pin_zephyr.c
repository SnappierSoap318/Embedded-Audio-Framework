#include "board_runtime.h"

int board_cpu_pin(k_tid_t thread, int cpu) {
    if (cpu < 0)
        return EAF_OK;
    return k_thread_cpu_pin(thread, cpu) ? EAF_IO : EAF_OK;
}
