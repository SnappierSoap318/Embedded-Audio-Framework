#define _POSIX_C_SOURCE 200809L
#include <eaf/eaf_hal.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <time.h>
typedef struct {
    pthread_t id;
    void (*entry)(void *);
    void *arg;
} thread_impl;
typedef struct {
    sem_t sem;
    atomic_bool pending;
} sem_impl;
static void *thread_entry(void *arg) {
    thread_impl *t = arg;
    t->entry(t->arg);
    return NULL;
}
int hal_thread_create(eaf_thread_t *thread, void (*entry)(void *), void *arg) {
    if (!thread || thread->impl || !entry)
        return EAF_INVALID;
    thread_impl *t = calloc(1, sizeof(*t));
    if (!t)
        return EAF_IO;
    t->entry = entry;
    t->arg = arg;
    if (pthread_create(&t->id, NULL, thread_entry, t)) {
        free(t);
        return EAF_IO;
    }
    thread->impl = t;
    return EAF_OK;
}
int hal_thread_join(eaf_thread_t *thread) {
    if (!thread || !thread->impl)
        return EAF_INVALID;
    thread_impl *t = thread->impl;
    if (pthread_join(t->id, NULL))
        return EAF_IO;
    free(t);
    thread->impl = NULL;
    return EAF_OK;
}
int hal_sem_init(eaf_sem_t *sem) {
    if (!sem || sem->impl)
        return EAF_INVALID;
    sem_impl *s = calloc(1, sizeof(*s));
    if (!s)
        return EAF_IO;
    atomic_init(&s->pending, false);
    if (sem_init(&s->sem, 0, 0)) {
        free(s);
        return EAF_IO;
    }
    sem->impl = s;
    return EAF_OK;
}
int hal_sem_take(eaf_sem_t *sem, uint32_t timeout_ms) {
    if (!sem || !sem->impl)
        return EAF_INVALID;
    sem_impl *s = sem->impl;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts))
        return EAF_IO;
    ts.tv_sec += (time_t)(timeout_ms / 1000u);
    ts.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ++ts.tv_sec;
        ts.tv_nsec -= 1000000000L;
    }
    int rc;
    do {
        rc = sem_timedwait(&s->sem, &ts);
    } while (rc && errno == EINTR);
    if (rc)
        return EAF_IO;
    atomic_store_explicit(&s->pending, false, memory_order_release);
    return EAF_OK;
}
void hal_sem_give(eaf_sem_t *sem) {
    sem_impl *s = sem->impl;
    if (!atomic_exchange_explicit(&s->pending, true, memory_order_acq_rel))
        (void)sem_post(&s->sem);
}
void hal_sem_deinit(eaf_sem_t *sem) {
    if (!sem || !sem->impl)
        return;
    sem_impl *s = sem->impl;
    (void)sem_destroy(&s->sem);
    free(s);
    sem->impl = NULL;
}
uint64_t hal_monotonic_time_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000) + (uint64_t)ts.tv_nsec / 1000u;
}
int hal_sleep_until_us(uint64_t deadline) {
    struct timespec ts = {(time_t)(deadline / 1000000u), (long)(deadline % 1000000u) * 1000L};
    int rc;
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    } while (rc == EINTR);
    return rc ? EAF_IO : EAF_OK;
}
void hal_sleep_ms(uint32_t ms) {
    (void)hal_sleep_until_us(hal_monotonic_time_us() + (uint64_t)ms * 1000u);
}
