#include "check.h"
#include <eaf/eaf_reservoir.h>
#include <pthread.h>
#include <sched.h>
#define TOTAL 1048576u
static eaf_reservoir_t r;
static int32_t storage[4096u * 2u];
static void *producer(void *unused) {
    (void)unused;
    uint32_t next = 0, random = 42;
    int32_t block[514];
    while (next < TOTAL) {
        random = random * 1664525u + 1013904223u;
        uint32_t n = 1u + random % 257u;
        if (n > TOTAL - next)
            n = TOTAL - next;
        for (uint32_t i = 0; i < n; ++i) {
            block[(size_t)i * 2u] = (int32_t)(next + i);
            block[(size_t)i * 2u + 1u] = -(int32_t)(next + i);
        }
        uint32_t written = eaf_reservoir_write(&r, block, n);
        next += written;
        if (!written)
            (void)sched_yield();
    }
    return NULL;
}
int main(void) {
    eaf_format_t fmt = {48000, 2, 3};
    CHECK(eaf_reservoir_init(&r, storage, 4096, fmt, 64) == 0);
    /* Exercise unsigned cursor rollover during concurrent traffic. */
    hal_atomic_set(&r.read_cursor, UINT32_MAX - 2047u);
    hal_atomic_set(&r.write_cursor, UINT32_MAX - 2047u);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, producer, NULL) == 0);
    int32_t out[128];
    eaf_buffer_t b = {out, 64, 64, 128, fmt, 0};
    for (uint32_t next = 0; next < TOTAL; next += 64u) {
        while (eaf_reservoir_level(&r) < 64u)
            (void)sched_yield();
        CHECK(eaf_reservoir_pull(&r, &b) == 0);
        for (uint32_t i = 0; i < 64u; ++i) {
            CHECK(out[(size_t)i * 2u] == (int32_t)(next + i));
            CHECK(out[(size_t)i * 2u + 1u] == -(int32_t)(next + i));
        }
    }
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(r.underruns == 0 && eaf_reservoir_level(&r) == 0);
    return 0;
}
