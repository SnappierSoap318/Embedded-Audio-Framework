/* Exercise the actual native LMS lifecycle, including its static callbacks. */
#define main eaf_lms_cli_main
// Test-only inclusion exercises static production callbacks without copying lifecycle logic.
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../platform/native_linux/play_lms.c"
#undef main
#include "check.h"

static atomic_bool heap_guard;
static bool reject_create, reject_start, reject_join, reject_stop;
int __real_hal_thread_join(eaf_thread_t *);
int __wrap_hal_thread_join(eaf_thread_t *thread) {
    return reject_join ? EAF_IO : __real_hal_thread_join(thread);
}
static int guarded_stop(eaf_sink_t *s) {
    return reject_stop ? EAF_IO : eaf_null_sink_ops.stop(s);
}
static atomic_uint commits;
int __real_hal_thread_create(eaf_thread_t *, void (*)(void *), void *);
int __wrap_hal_thread_create(eaf_thread_t *thread, void (*entry)(void *), void *ctx) {
    CHECK(pipeline.state == EAF_CONFIGURED);
    return reject_create ? EAF_IO : __real_hal_thread_create(thread, entry, ctx);
}
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
void *__wrap_malloc(size_t n) {
    CHECK(!atomic_load(&heap_guard));
    return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t size) {
    CHECK(!atomic_load(&heap_guard));
    return __real_calloc(n, size);
}
void *__wrap_realloc(void *p, size_t n) {
    CHECK(!atomic_load(&heap_guard));
    return __real_realloc(p, n);
}
void __wrap_free(void *p) {
    CHECK(!atomic_load(&heap_guard));
    __real_free(p);
}
static int guarded_start(eaf_sink_t *s) {
    CHECK(audio.impl != NULL);
    CHECK(atomic_load(&commits) == 0);
    if (reject_start)
        return EAF_IO;
    int rc = eaf_null_sink_ops.start(s);
    atomic_store(&heap_guard, true);
    return rc;
}
static int guarded_commit(eaf_sink_t *s, eaf_buffer_t *b) {
    CHECK(pipeline.state == EAF_RUNNING);
    int rc = eaf_null_sink_ops.commit_buf(s, b);
    atomic_fetch_add(&commits, 1u);
    return rc;
}
int main(void) {
    struct eaf_sink_ops ops = eaf_null_sink_ops;
    ops.start = guarded_start;
    ops.stop = guarded_stop;
    ops.commit_buf = guarded_commit;
    sink.ops = &ops;
    eaf_format_t fmt = {48000, 2, 3};
    reject_create = true;
    CHECK(start(NULL, &fmt) == EAF_IO);
    CHECK(!active && !audio.impl && pipeline.state == EAF_UNINITIALIZED);
    reject_create = false;
    reject_start = true;
    CHECK(start(NULL, &fmt) == EAF_IO);
    CHECK(!active && !audio.impl && pipeline.state == EAF_UNINITIALIZED);
    CHECK(atomic_load(&commits) == 0);
    reject_start = false;
    for (unsigned track = 0; track < 8; ++track) {
        fmt.sample_rate = track & 1u ? 44100 : 48000;
        atomic_store(&commits, 0);
        CHECK(start(NULL, &fmt) == 0);
        uint64_t deadline = hal_monotonic_time_us() + 1000000u;
        while (!atomic_load(&commits)) {
            CHECK(hal_monotonic_time_us() < deadline);
            hal_sleep_ms(1);
        }
        CHECK(!hal_atomic_get(&failed));
        atomic_store(&heap_guard, false); /* Teardown is outside the processing guard. */
        if (track == 0) {
            reject_join = true;
            stop(NULL);
            CHECK(active && audio.impl && pipeline.state == EAF_RUNNING);
            CHECK(start(NULL, &fmt) == EAF_STATE);
            reject_join = false;
        }
        if (track == 1) {
            reject_stop = true;
            stop(NULL);
            CHECK(active && !audio.impl && pipeline.state == EAF_RUNNING);
            CHECK(start(NULL, &fmt) == EAF_STATE);
            reject_stop = false;
        }
        stop(NULL);
        CHECK(!active && !audio.impl && pipeline.state == EAF_UNINITIALIZED);
        CHECK(hal_atomic_get(&done));
    }
    return 0;
}
