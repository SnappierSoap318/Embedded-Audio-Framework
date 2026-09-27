#include "check.h"
#include <eaf/eaf_sink_i2s_core.h>
#include <string.h>

typedef struct {
    int32_t block[128u * 2u];
    unsigned writes, drains, drops, disables, deletes, allocs, frees, starts, enables;
    bool fail_configure, fail_enable, fail_write, fail_start, fail_drop, fail_drain, fail_disable,
        fail_delete, fail_alloc;
} fake_ctx_t;

static fake_ctx_t fake;
static char last_operation[32];
static unsigned last_submitted;
static bool handler_called;

static void on_error(const char *operation, int error, uint32_t submitted_blocks) {
    (void)error;
    handler_called = true;
    last_submitted = submitted_blocks;
    (void)snprintf(last_operation, sizeof(last_operation), "%s", operation);
}

static int fake_configure(eaf_i2s_core_t *core, const eaf_format_t *fmt, size_t frames) {
    (void)core;
    (void)fmt;
    (void)frames;
    return fake.fail_configure ? EAF_IO : EAF_OK;
}

static int fake_enable(eaf_i2s_core_t *core) {
    (void)core;
    ++fake.enables;
    return fake.fail_enable ? EAF_IO : EAF_OK;
}

static int fake_write(eaf_i2s_core_t *core, const void *data, size_t bytes, size_t *written) {
    (void)core;
    (void)data;
    ++fake.writes;
    if (fake.fail_write)
        return EAF_IO;
    *written = bytes;
    return EAF_OK;
}

static int fake_start(eaf_i2s_core_t *core) {
    (void)core;
    ++fake.starts;
    return fake.fail_start ? EAF_IO : EAF_OK;
}

static int fake_drop(eaf_i2s_core_t *core) {
    (void)core;
    ++fake.drops;
    return fake.fail_drop ? EAF_IO : EAF_OK;
}

static int fake_drain(eaf_i2s_core_t *core) {
    (void)core;
    ++fake.drains;
    return fake.fail_drain ? EAF_IO : EAF_OK;
}

static int fake_disable(eaf_i2s_core_t *core) {
    (void)core;
    ++fake.disables;
    return fake.fail_disable ? EAF_IO : EAF_OK;
}

static int fake_delete(eaf_i2s_core_t *core) {
    (void)core;
    ++fake.deletes;
    return fake.fail_delete ? EAF_IO : EAF_OK;
}

static void *fake_dma_alloc(eaf_i2s_core_t *core, size_t bytes) {
    (void)core;
    (void)bytes;
    ++fake.allocs;
    return fake.fail_alloc ? NULL : fake.block;
}

static void fake_dma_free(eaf_i2s_core_t *core, void *block) {
    (void)core;
    if (block)
        ++fake.frees;
}

static const eaf_i2s_backend_t fake_backend = {.configure = fake_configure,
                                               .enable = fake_enable,
                                               .write = fake_write,
                                               .start = fake_start,
                                               .drop = fake_drop,
                                               .drain = fake_drain,
                                               .disable = fake_disable,
                                               .delete_channel = fake_delete,
                                               .tune_ppm = NULL,
                                               .dma_alloc = fake_dma_alloc,
                                               .dma_free = fake_dma_free,
                                               .enable_before_write = false};

static eaf_i2s_core_t sink_core = {.backend = &fake_backend, .ctx = &fake};
static eaf_sink_t sink = {&eaf_i2s_core_sink_ops, &sink_core};

static bool operation_is(const char *name) {
    return handler_called && !strcmp(last_operation, name);
}

int main(void) {
    eaf_i2s_core_set_error_handler(&sink_core, on_error);
    eaf_format_t fmt = {48000, 2, 3};
    eaf_buffer_t *buf;
    CHECK(sink.ops->init(&sink, &fmt, 0) == EAF_INVALID);
    CHECK(sink.ops->init(&sink, &fmt, 128) == EAF_OK);
    CHECK(sink_core.configured && !sink_core.running);
    CHECK(sink.ops->init(&sink, &fmt, 128) == EAF_STATE);
    CHECK(sink.ops->acquire_buf(&sink, &buf) == EAF_STATE);
    CHECK(sink.ops->start(&sink) == EAF_OK);
    CHECK(sink.ops->start(&sink) == EAF_STATE);
    CHECK(sink.ops->acquire_buf(&sink, &buf) == EAF_OK);
    CHECK(sink_core.held && buf == &sink_core.buffer && buf->samples == fake.block);
    eaf_buffer_t other = {0};
    CHECK(sink.ops->commit_buf(&sink, &other) == EAF_STATE);
    CHECK(sink.ops->commit_buf(&sink, buf) == EAF_OK);
    CHECK(!sink_core.held && sink_core.submitted_blocks == 1u);
    CHECK(fake.writes == 1u && fake.starts == 1u && sink_core.started);
    CHECK(sink.ops->commit_buf(&sink, buf) == EAF_STATE);
    CHECK(sink.ops->adjust_ppm(&sink, 10) == EAF_UNSUPPORTED);
    /* Pause drains queued DMA but preserves PCM and gating. */
    CHECK(sink.ops->acquire_buf(&sink, &buf) == EAF_OK);
    CHECK(eaf_i2s_core_pause(&sink_core, true) == EAF_STATE);
    CHECK(sink.ops->commit_buf(&sink, buf) == EAF_OK);
    CHECK(eaf_i2s_core_pause(&sink_core, true) == EAF_OK);
    CHECK(sink_core.paused && !sink_core.started && fake.drains == 1u);
    CHECK(sink.ops->acquire_buf(&sink, &buf) == EAF_STATE);
    CHECK(eaf_i2s_core_pause(&sink_core, false) == EAF_OK);
    CHECK(!sink_core.paused);
    /* A failed write retains the held block for a checked stop. */
    fake.fail_write = true;
    handler_called = false;
    CHECK(sink.ops->acquire_buf(&sink, &buf) == EAF_OK);
    CHECK(sink.ops->commit_buf(&sink, buf) == EAF_IO);
    CHECK(sink_core.held && operation_is("write"));
    fake.fail_write = false;
    CHECK(sink.ops->stop(&sink) == EAF_OK);
    CHECK(!sink_core.held && !sink_core.running && fake.frees == 1u);
    /* A failed drain retains started state until the driver recovers. */
    CHECK(sink.ops->start(&sink) == EAF_OK);
    CHECK(sink.ops->acquire_buf(&sink, &buf) == EAF_OK);
    CHECK(sink.ops->commit_buf(&sink, buf) == EAF_OK);
    CHECK(sink_core.started);
    fake.fail_drain = true;
    handler_called = false;
    CHECK(sink.ops->stop(&sink) == EAF_IO);
    CHECK(sink_core.started && operation_is("drain"));
    fake.fail_drain = false;
    CHECK(sink.ops->stop(&sink) == EAF_OK);
    CHECK(!sink_core.started && !sink_core.running);
    /* A failed delete keeps the context retry-safe. */
    fake.fail_delete = true;
    handler_called = false;
    CHECK(sink.ops->deinit(&sink) == EAF_IO);
    CHECK(sink_core.configured && operation_is("delete"));
    fake.fail_delete = false;
    CHECK(sink.ops->deinit(&sink) == EAF_OK);
    CHECK(!sink_core.configured);
    /* Allocation failure reports and leaves nothing held. */
    CHECK(sink.ops->init(&sink, &fmt, 128) == EAF_OK);
    CHECK(sink.ops->start(&sink) == EAF_OK);
    fake.fail_alloc = true;
    handler_called = false;
    CHECK(sink.ops->acquire_buf(&sink, &buf) == EAF_IO);
    CHECK(!sink_core.held && operation_is("allocate"));
    fake.fail_alloc = false;
    CHECK(sink.ops->deinit(&sink) == EAF_OK);
    /* Configure failure leaves the sink uninitialized. */
    fake.fail_configure = true;
    CHECK(sink.ops->init(&sink, &fmt, 128) == EAF_IO);
    CHECK(!sink_core.configured);
    CHECK(last_submitted == 0u);
    return 0;
}
