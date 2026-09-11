#define _GNU_SOURCE
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
    const eaf_thread_options_t options = {EAF_THREAD_DECODER, -1, false};
    return hal_thread_create_with_options(thread, entry, arg, &options);
}
int hal_thread_create_with_options(eaf_thread_t *thread, void (*entry)(void *), void *arg,
                                   const eaf_thread_options_t *options) {
    if (!thread || thread->impl || !entry || !options ||
        (options->role != EAF_THREAD_DECODER && options->role != EAF_THREAD_AUDIO) ||
        options->cpu < -1 || options->cpu >= CPU_SETSIZE)
        return EAF_INVALID;
    thread_impl *t = calloc(1, sizeof(*t));
    if (!t)
        return EAF_IO;
    t->entry = entry;
    t->arg = arg;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr)) {
        free(t);
        return EAF_IO;
    }
    /* Explicit ordinary policy avoids accidentally inheriting an RT creator. */
    struct sched_param scheduling = {
        .sched_priority = options->realtime ? (options->role == EAF_THREAD_AUDIO ? 20 : 10) : 0};
    int rc = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    if (!rc)
        rc = pthread_attr_setschedpolicy(&attr, options->realtime ? SCHED_FIFO : SCHED_OTHER);
    if (!rc)
        rc = pthread_attr_setschedparam(&attr, &scheduling);
    if (!rc && options->cpu >= 0) {
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        CPU_SET((unsigned)options->cpu, &cpus);
        rc = pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);
    }
    if (!rc)
        rc = pthread_create(&t->id, &attr, thread_entry, t);
    (void)pthread_attr_destroy(&attr);
    if (rc) {
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
    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        return EAF_IO;
    ts.tv_sec += (time_t)(timeout_ms / 1000u);
    ts.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ++ts.tv_sec;
        ts.tv_nsec -= 1000000000L;
    }
    int rc;
    do {
        rc = sem_clockwait(&s->sem, CLOCK_MONOTONIC, &ts);
    } while (rc && errno == EINTR);
    if (rc)
        return errno == ETIMEDOUT ? EAF_TIMEOUT : EAF_IO;
    atomic_store_explicit(&s->pending, false, memory_order_release);
    return EAF_OK;
}
void hal_sem_give(eaf_sem_t *sem) {
    if (!sem || !sem->impl)
        return;
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
