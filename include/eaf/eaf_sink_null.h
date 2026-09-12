#pragma once
#include <eaf/eaf_hal.h>
#ifndef EAF_NULL_MAX_FRAMES
#define EAF_NULL_MAX_FRAMES 1024u
#endif
typedef struct {
    _Alignas(32) int32_t samples[2][EAF_NULL_MAX_FRAMES * EAF_MAX_CHANNELS];
    eaf_buffer_t buffers[2];
    uint64_t epoch_us, frames_committed;
    unsigned active;
    bool initialized, running, acquired;
} eaf_null_sink_ctx_t;
extern const struct eaf_sink_ops eaf_null_sink_ops;
