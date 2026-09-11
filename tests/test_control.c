#include "check.h"
#include <eaf/eaf_control.h>
#include <pthread.h>
#include <sched.h>
static eaf_control_queue_t q;
static void *produce(void *arg) {
    (void)arg;
    for (uint32_t i = 0; i < 100000u; ++i) {
        eaf_command_t c = {EAF_COMMAND_SEEK, UINT64_C(0x100000000) + i, (int32_t)i};
        while (!eaf_control_push(&q, &c))
            (void)sched_yield();
    }
    return NULL;
}
int main(void) {
    eaf_control_init(&q);
    eaf_command_t c = {EAF_COMMAND_STOP, 0, 0}, out;
    CHECK(!eaf_control_pop(&q, &out));
    for (unsigned i = 0; i < EAF_CONTROL_CAPACITY; ++i)
        CHECK(eaf_control_push(&q, &c));
    CHECK(!eaf_control_push(&q, &c));
    for (unsigned i = 0; i < EAF_CONTROL_CAPACITY; ++i)
        CHECK(eaf_control_pop(&q, &out));
    CHECK(!eaf_control_pop(&q, &out));
    hal_atomic_set(&q.read_cursor, UINT32_MAX - 7u);
    hal_atomic_set(&q.write_cursor, UINT32_MAX - 7u);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, produce, NULL) == 0);
    for (uint32_t i = 0; i < 100000u; ++i) {
        while (!eaf_control_pop(&q, &out))
            (void)sched_yield();
        CHECK(out.type == EAF_COMMAND_SEEK && out.frame == UINT64_C(0x100000000) + i &&
              out.gain == (int32_t)i);
    }
    CHECK(pthread_join(thread, NULL) == 0);
    return 0;
}
