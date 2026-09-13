#include "board_output.h"
#include <eaf/eaf_sink_null.h>

static eaf_null_sink_ctx_t context;
static eaf_sink_t sink = {&eaf_null_sink_ops, &context};
static uint64_t paused_at;

eaf_sink_t *board_sink(void) {
    return &sink;
}
int board_sink_init(void) {
    return EAF_OK;
}
int board_sink_pause(bool paused) {
    if (paused)
        paused_at = hal_monotonic_time_us();
    else
        context.epoch_us += hal_monotonic_time_us() - paused_at;
    return EAF_OK;
}
