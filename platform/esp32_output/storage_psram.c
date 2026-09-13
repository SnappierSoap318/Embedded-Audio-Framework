#include "board_output.h"
#include <zephyr/multi_heap/shared_multi_heap.h>

int eaf_board_output_storage(int32_t **out, uint32_t *frames) {
    int32_t *memory = shared_multi_heap_alloc(
        SMH_REG_ATTR_EXTERNAL, sizeof(int32_t) * CONFIG_EAF_BOARD_RESERVOIR_FRAMES * 2u);
    if (!memory)
        return EAF_IO;
    *out = memory;
    *frames = CONFIG_EAF_BOARD_RESERVOIR_FRAMES;
    return EAF_OK;
}
