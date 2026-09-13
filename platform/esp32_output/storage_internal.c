#include "board_output.h"

static int32_t storage[CONFIG_EAF_BOARD_RESERVOIR_FRAMES * 2u];

int eaf_board_output_storage(int32_t **out, uint32_t *frames) {
    *out = storage;
    *frames = CONFIG_EAF_BOARD_RESERVOIR_FRAMES;
    return EAF_OK;
}
