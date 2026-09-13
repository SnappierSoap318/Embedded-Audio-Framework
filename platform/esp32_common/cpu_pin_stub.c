#include "board_runtime.h"

int board_cpu_pin(k_tid_t thread, int cpu) {
    (void)thread;
    return cpu < 0 ? EAF_OK : EAF_UNSUPPORTED;
}
