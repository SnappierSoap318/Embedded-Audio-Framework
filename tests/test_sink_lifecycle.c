#include "check.h"
#include <eaf/eaf_core.h>
#include <eaf/eaf_sink_null.h>
static eaf_null_sink_ctx_t ctx;
static bool fail_init, fail_start, fail_stop, fail_deinit, fail_commit;
static int init(eaf_sink_t *s, const eaf_format_t *f, size_t n) {
    int rc = eaf_null_sink_ops.init(s, f, n);
    return rc ? rc : (fail_init ? EAF_IO : EAF_OK);
}
static int start(eaf_sink_t *s) {
    int rc = eaf_null_sink_ops.start(s);
    return rc ? rc : (fail_start ? EAF_IO : EAF_OK);
}
static int stop(eaf_sink_t *s) {
    return fail_stop ? EAF_IO : eaf_null_sink_ops.stop(s);
}
static int deinit(eaf_sink_t *s) {
    return fail_deinit ? EAF_IO : eaf_null_sink_ops.deinit(s);
}
static int commit(eaf_sink_t *s, eaf_buffer_t *b) {
    return fail_commit ? EAF_IO : eaf_null_sink_ops.commit_buf(s, b);
}
int main(void) {
    struct eaf_sink_ops ops = eaf_null_sink_ops;
    ops.init = init;
    ops.start = start;
    ops.stop = stop;
    ops.deinit = deinit;
    ops.commit_buf = commit;
    eaf_sink_t sink = {&ops, &ctx};
    eaf_pipeline_t p = {0};
    eaf_reservoir_t r;
    int32_t samples[128] = {0};
    CHECK(!eaf_reservoir_init(&r, samples, 64, (eaf_format_t){48000, 2, 3}, 32));
    eaf_pipeline_config_t config = {&r, NULL, 0, &sink};
    CHECK(!eaf_pipeline_init(&p, &config));
    fail_init = fail_deinit = true;
    CHECK(eaf_pipeline_configure(&p, 16) == EAF_IO);
    CHECK(p.state == EAF_RECOVERY && p.sink_initialized && !p.sink_ready);
    CHECK(eaf_pipeline_start(&p) == EAF_STATE);
    CHECK(eaf_pipeline_configure(&p, 16) == EAF_STATE);
    CHECK(eaf_pipeline_deinit(&p) == EAF_IO);
    CHECK(!eaf_pipeline_stop(&p));
    CHECK(p.state == EAF_INITIALIZED);
    CHECK(eaf_pipeline_start(&p) == EAF_STATE);
    fail_init = fail_deinit = false;
    CHECK(!eaf_pipeline_configure(&p, 16));
    fail_start = fail_stop = true;
    CHECK(eaf_pipeline_start(&p) == EAF_IO);
    CHECK(p.state == EAF_RECOVERY && ctx.running);
    CHECK(eaf_pipeline_start(&p) == EAF_STATE);
    fail_stop = false;
    CHECK(!eaf_pipeline_stop(&p));
    CHECK(p.state == EAF_STOPPED && !ctx.running);
    /* START error after side effects must roll back even without an explicit stop. */
    CHECK(eaf_pipeline_start(&p) == EAF_IO);
    CHECK(p.state == EAF_STOPPED && !ctx.running);
    fail_start = false;
    CHECK(!eaf_pipeline_start(&p));
    eaf_reservoir_finish(&r);
    fail_commit = true;
    CHECK(eaf_pipeline_process(&p) == EAF_IO);
    CHECK(!p.completed && ctx.acquired);
    fail_stop = true;
    CHECK(eaf_pipeline_stop(&p) == EAF_IO);
    CHECK(p.state == EAF_RUNNING && ctx.acquired);
    CHECK(eaf_pipeline_deinit(&p) == EAF_STATE);
    fail_stop = fail_commit = false;
    CHECK(!eaf_pipeline_stop(&p));
    CHECK(!ctx.acquired);
    fail_deinit = true;
    CHECK(eaf_pipeline_configure(&p, 32) == EAF_IO);
    CHECK(p.state == EAF_RECOVERY && p.sink_initialized && !p.sink_ready);
    CHECK(eaf_pipeline_start(&p) == EAF_STATE);
    fail_deinit = false;
    CHECK(!eaf_pipeline_deinit(&p));
    CHECK(p.state == EAF_UNINITIALIZED && !p.sink_initialized);
    CHECK(!eaf_pipeline_deinit(&p));
    return 0;
}
