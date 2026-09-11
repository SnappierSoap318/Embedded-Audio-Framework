#define _GNU_SOURCE
#include "check.h"
#include <eaf/eaf_hal.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
static bool interrupt_wait;
static struct timespec interrupted_deadline;
int __real_sem_clockwait(sem_t *, clockid_t, const struct timespec *);
int __wrap_sem_clockwait(sem_t *sem, clockid_t clock, const struct timespec *deadline) {
    CHECK(clock == CLOCK_MONOTONIC);
    if (interrupt_wait) {
        interrupt_wait = false;
        interrupted_deadline = *deadline;
        errno = EINTR;
        return -1;
    }
    if (interrupted_deadline.tv_sec) {
        CHECK(deadline->tv_sec == interrupted_deadline.tv_sec);
        CHECK(deadline->tv_nsec == interrupted_deadline.tv_nsec);
        interrupted_deadline = (struct timespec){0};
    }
    return __real_sem_clockwait(sem, clock, deadline);
}
static int expected_fifo;
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg) {
    int policy, inherit;
    struct sched_param param;
    CHECK(!pthread_attr_getschedpolicy(a, &policy));
    CHECK(!pthread_attr_getinheritsched(a, &inherit));
    CHECK(!pthread_attr_getschedparam(a, &param));
    CHECK(inherit == PTHREAD_EXPLICIT_SCHED);
    CHECK(policy == (expected_fifo ? SCHED_FIFO : SCHED_OTHER));
    CHECK(param.sched_priority == expected_fifo);
    if (expected_fifo)
        return EPERM; /* Deterministic denial: no privileges or RT CPU monopolization. */
    return __real_pthread_create(t, a, fn, arg);
}
static eaf_sem_t wake, ack;
static atomic_uint sequence, callbacks;
static int selected_cpu = -1;
static void worker(void *arg) {
    (void)arg;
    atomic_fetch_add(&callbacks, 1u);
    if (selected_cpu >= 0) {
        cpu_set_t cpus;
        CHECK(!pthread_getaffinity_np(pthread_self(), sizeof(cpus), &cpus));
        CHECK(CPU_COUNT(&cpus) == 1 && CPU_ISSET((unsigned)selected_cpu, &cpus));
        CHECK(sched_getcpu() == selected_cpu);
    }
    for (unsigned i = 1; i <= 5000; ++i) {
        atomic_store(&sequence, i);
        hal_sem_give(&wake);
        CHECK(!hal_sem_take(&ack, 1000));
    }
}
int main(void) {
    hal_sem_give(NULL);
    CHECK(hal_sem_take(NULL, 0) == EAF_INVALID);
    CHECK(!hal_sem_init(&wake));
    CHECK(!hal_sem_init(&ack));
    CHECK(hal_sem_init(&wake) == EAF_INVALID);
    CHECK(hal_sem_take(&wake, 0) == EAF_TIMEOUT);
    for (unsigned i = 0; i < 10000; ++i)
        hal_sem_give(&wake);
    CHECK(!hal_sem_take(&wake, 0));
    CHECK(hal_sem_take(&wake, 0) == EAF_TIMEOUT);
    uint64_t before = hal_monotonic_time_us();
    interrupt_wait = true;
    CHECK(hal_sem_take(&wake, 20) == EAF_TIMEOUT);
    CHECK(hal_monotonic_time_us() - before >= 20000u);
    CHECK(!hal_sleep_until_us(before));
    before = hal_monotonic_time_us();
    CHECK(!hal_sleep_until_us(before + 2000u));
    CHECK(hal_monotonic_time_us() >= before + 2000u);
    eaf_thread_t thread = {0};
    eaf_thread_options_t options = {EAF_THREAD_AUDIO, -1, true};
    expected_fifo = 20;
    CHECK(hal_thread_create_with_options(&thread, worker, NULL, &options) == EAF_IO);
    CHECK(!thread.impl && !atomic_load(&callbacks));
    options.role = EAF_THREAD_DECODER;
    expected_fifo = 10;
    CHECK(hal_thread_create_with_options(&thread, worker, NULL, &options) == EAF_IO);
    CHECK(!thread.impl && !atomic_load(&callbacks));
    expected_fifo = 0;
    options.cpu = -2;
    CHECK(hal_thread_create_with_options(&thread, worker, NULL, &options) == EAF_INVALID);
    CHECK(hal_thread_create_with_options(&thread, worker, NULL, NULL) == EAF_INVALID);
    cpu_set_t allowed;
    CHECK(!sched_getaffinity(0, sizeof(allowed), &allowed));
    for (int i = 0; i < CPU_SETSIZE; ++i) {
        if (CPU_ISSET((unsigned)i, &allowed)) {
            selected_cpu = i;
            break;
        }
    }
    CHECK(selected_cpu >= 0);
    options = (eaf_thread_options_t){EAF_THREAD_AUDIO, selected_cpu, false};
    CHECK(!hal_thread_create_with_options(&thread, worker, NULL, &options));
    CHECK(hal_thread_create(&thread, worker, NULL) == EAF_INVALID);
    for (unsigned i = 1; i <= 5000; ++i) {
        CHECK(!hal_sem_take(&wake, 1000));
        CHECK(atomic_load(&sequence) == i);
        hal_sem_give(&ack);
    }
    CHECK(!hal_thread_join(&thread));
    CHECK(!thread.impl && atomic_load(&callbacks) == 1);
    CHECK(hal_thread_join(&thread) == EAF_INVALID);
    CHECK(hal_sem_take(&wake, 0) == EAF_TIMEOUT);
    hal_sem_deinit(&wake);
    hal_sem_deinit(&ack);
    hal_sem_deinit(&wake);
    return 0;
}
