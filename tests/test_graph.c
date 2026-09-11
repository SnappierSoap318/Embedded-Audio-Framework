#include "check.h"
#include <eaf/eaf_dsp.h>
#include <eaf/eaf_sink_null.h>
static bool forbid_heap;
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
void __real_free(void *ptr);
void *__wrap_malloc(size_t size) {
    CHECK(!forbid_heap);
    return __real_malloc(size);
}
void *__wrap_calloc(size_t count, size_t size) {
    CHECK(!forbid_heap);
    return __real_calloc(count, size);
}
void *__wrap_realloc(void *ptr, size_t size) {
    CHECK(!forbid_heap);
    return __real_realloc(ptr, size);
}
void __wrap_free(void *ptr) {
    CHECK(!forbid_heap);
    __real_free(ptr);
}
static unsigned initialized, deinitialized, resets;
static bool fail_init, fail_process;
static unsigned corrupt;
static int node_init(eaf_node_t *n, const eaf_format_t *in, eaf_format_t *out) {
    (void)n;
    ++initialized;
    *out = *in;
    return fail_init ? EAF_IO : EAF_OK;
}
static int node_process(eaf_node_t *n, eaf_buffer_t *b) {
    (void)n;
    switch (corrupt) {
    case 1:
        b->samples = NULL;
        break;
    case 2:
        b->capacity_samples = 1;
        break;
    case 3:
        b->frame_count = UINT32_MAX;
        break;
    case 4:
        b->format.num_channels = 0;
        break;
    case 5:
        b->flags = 0;
        break;
    default:
        break;
    }
    return fail_process ? EAF_IO : EAF_OK;
}
static int node_reset(eaf_node_t *n) {
    (void)n;
    ++resets;
    return EAF_OK;
}
static void node_deinit(eaf_node_t *n) {
    (void)n;
    ++deinitialized;
}
static const struct eaf_node_ops ops = {node_init, node_process, node_reset, node_deinit};
static eaf_null_sink_ctx_t sink_ctx;
int main(void) {
    eaf_pipeline_t p = {0};
    eaf_reservoir_t r;
    int32_t storage[128], src[128];
    for (size_t i = 0; i < 128; ++i)
        src[i] = 42;
    eaf_format_t fmt = {48000, 2, 3};
    CHECK(eaf_reservoir_init(&r, storage, 64, fmt, 32) == 0);
    eaf_sink_t sink = {&eaf_null_sink_ops, &sink_ctx};
    eaf_node_t n = {"test", EAF_NODE_STAGE_DSP, &ops, NULL};
    eaf_node_t *nodes[] = {&n};
    eaf_pipeline_config_t config = {&r, nodes, 1, &sink};
    CHECK(eaf_pipeline_start(&p) == EAF_STATE);
    CHECK(eaf_pipeline_init(&p, &config) == 0);
    CHECK(eaf_pipeline_init(&p, &config) == EAF_STATE);
    fail_init = true;
    CHECK(eaf_pipeline_configure(&p, 16) == EAF_IO);
    CHECK(initialized == 1 && deinitialized == 1 && p.state == EAF_INITIALIZED);
    fail_init = false;
    CHECK(eaf_pipeline_configure(&p, 16) == 0);
    CHECK(eaf_pipeline_start(&p) == 0);
    forbid_heap = true;
    CHECK(eaf_pipeline_start(&p) == EAF_STATE);
    CHECK(eaf_pipeline_configure(&p, 16) == EAF_STATE);
    CHECK(eaf_pipeline_deinit(&p) == EAF_STATE);
    CHECK(eaf_reservoir_write(&r, src, 64) == 64);
    CHECK(eaf_pipeline_process(&p) == 0);
    CHECK(sink_ctx.samples[0][0] == 42);
    fail_process = true;
    CHECK(eaf_pipeline_process(&p) == EAF_IO);
    for (size_t i = 0; i < 32; ++i)
        CHECK(sink_ctx.samples[1][i] == 0);
    CHECK(!sink_ctx.acquired);
    fail_process = false;
    for (corrupt = 1; corrupt <= 4; ++corrupt) {
        unsigned slot = sink_ctx.active;
        CHECK(eaf_pipeline_process(&p) == EAF_INVALID);
        CHECK(!sink_ctx.acquired);
        CHECK(sink_ctx.buffers[slot].samples == sink_ctx.samples[slot]);
        CHECK(sink_ctx.buffers[slot].capacity_samples ==
              (size_t)EAF_NULL_MAX_FRAMES * EAF_MAX_CHANNELS);
        for (size_t i = 0; i < 32; ++i)
            CHECK(sink_ctx.samples[slot][i] == 0);
    }
    corrupt = 5;
    eaf_reservoir_finish(&r);
    unsigned eos_slot = sink_ctx.active;
    CHECK(eaf_pipeline_process(&p) == EAF_EOF);
    CHECK(sink_ctx.buffers[eos_slot].flags & EAF_FRAME_EOS);
    corrupt = 0;
    CHECK(eaf_pipeline_stop(&p) == 0 && resets == 1);
    forbid_heap = false;
    CHECK(eaf_pipeline_process(&p) == EAF_STATE);
    CHECK(eaf_pipeline_start(&p) == 0);
    CHECK(eaf_reservoir_level(&r) == 0);
    CHECK(eaf_pipeline_stop(&p) == 0);
    r.format.sample_rate = 44100;
    CHECK(eaf_pipeline_configure(&p, 32) == 0);
    CHECK(p.output_format.sample_rate == 44100);
    CHECK(eaf_pipeline_deinit(&p) == 0);
    CHECK(initialized == deinitialized);
    /* Exercise real DSP and channel expansion under the heap guard. */
    eaf_crossover_2_1_ctx_t cross_ctx = {.frequency_hz = 80};
    eaf_eq_ctx_t eq_ctx = {.bands = 1, .coeff = {{1073741824, 0, 0, 0, 0}}};
    eaf_volume_ctx_t vol_ctx = {{1073741824, 1073741824, 1073741824, 0}};
    eaf_node_t cross = {"cross", EAF_NODE_STAGE_PRE_PROCESS, &eaf_crossover_2_1_ops, &cross_ctx};
    eaf_node_t eq = {"eq", EAF_NODE_STAGE_DSP, &eaf_biquad_eq_ops, &eq_ctx};
    eaf_node_t vol = {"volume", EAF_NODE_STAGE_POST_PROCESS, &eaf_volume_ops, &vol_ctx};
    eaf_node_t *real_nodes[] = {&cross, &eq, &vol};
    config.nodes = real_nodes;
    config.node_count = 3;
    CHECK(eaf_pipeline_init(&p, &config) == 0);
    CHECK(eaf_pipeline_configure(&p, 16) == 0);
    CHECK(p.output_format.num_channels == 3);
    CHECK(eaf_pipeline_start(&p) == 0);
    forbid_heap = true;
    CHECK(eaf_reservoir_write(&r, src, 64) == 64);
    CHECK(eaf_pipeline_process(&p) == 0);
    eaf_reservoir_finish(&r);
    CHECK(eaf_pipeline_process(&p) == 0);
    CHECK(eaf_pipeline_process(&p) == 0);
    CHECK(eaf_pipeline_process(&p) == EAF_EOF);
    uint64_t committed = sink_ctx.frames_committed;
    CHECK(eaf_pipeline_process(&p) == EAF_EOF && sink_ctx.frames_committed == committed);
    CHECK(eaf_pipeline_stop(&p) == 0);
    forbid_heap = false;
    CHECK(eaf_pipeline_deinit(&p) == 0);
    /* An invalid stage ordering is rejected before node initialization. */
    eaf_node_t earlier = {"earlier", EAF_NODE_STAGE_PRE_PROCESS, &ops, NULL};
    eaf_node_t *bad[] = {&n, &earlier};
    config.nodes = bad;
    config.node_count = 2;
    CHECK(eaf_pipeline_init(&p, &config) == EAF_INVALID);
    return 0;
}
