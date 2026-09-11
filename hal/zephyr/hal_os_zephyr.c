#include <eaf/eaf_hal.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
BUILD_ASSERT(CONFIG_EAF_AUDIO_PRIORITY < CONFIG_EAF_DECODER_PRIORITY,
             "Audio must outrank the decoder");
BUILD_ASSERT(CONFIG_EAF_DECODER_PRIORITY < CONFIG_NUM_PREEMPT_PRIORITIES,
             "EAF worker priority must exist in the kernel configuration");
/* Statically backed pools; exhaustion is explicit, never falls back to heap. */
typedef struct {
    struct k_thread thread;
    atomic_t busy;
    void (*entry)(void *);
    void *arg;
} thread_slot;
typedef struct {
    struct k_sem sem;
    atomic_t busy;
} sem_slot;
static thread_slot threads[CONFIG_EAF_THREAD_COUNT];
static sem_slot sems[CONFIG_EAF_SEM_COUNT];
K_THREAD_STACK_ARRAY_DEFINE(stacks, CONFIG_EAF_THREAD_COUNT, CONFIG_EAF_THREAD_STACK_SIZE);
static void entry_bridge(void *a, void *b, void *c) {
    (void)b;
    (void)c;
    thread_slot *slot = a;
    slot->entry(slot->arg);
}
int hal_thread_create(eaf_thread_t *thread, void (*entry)(void *), void *arg) {
    const eaf_thread_options_t options = {EAF_THREAD_DECODER, -1, false};
    return hal_thread_create_with_options(thread, entry, arg, &options);
}
int hal_thread_create_with_options(eaf_thread_t *thread, void (*entry)(void *), void *arg,
                                   const eaf_thread_options_t *options) {
    if (!thread || thread->impl || !entry || !options ||
        (options->role != EAF_THREAD_DECODER && options->role != EAF_THREAD_AUDIO) ||
        options->cpu < -1 || options->cpu >= CONFIG_MP_MAX_NUM_CPUS)
        return EAF_INVALID;
#ifndef CONFIG_SCHED_CPU_MASK
    if (options->cpu >= 0)
        return EAF_UNSUPPORTED;
#endif
    int priority =
        options->role == EAF_THREAD_AUDIO ? CONFIG_EAF_AUDIO_PRIORITY : CONFIG_EAF_DECODER_PRIORITY;
    for (size_t i = 0; i < CONFIG_EAF_THREAD_COUNT; ++i) {
        thread_slot *s = &threads[i];
        if (!atomic_cas(&s->busy, 0, 1))
            continue;
        s->entry = entry;
        s->arg = arg;
        k_tid_t id =
            k_thread_create(&s->thread, stacks[i], K_THREAD_STACK_SIZEOF(stacks[i]), entry_bridge,
                            s, NULL, NULL, K_PRIO_PREEMPT(priority), 0, K_FOREVER);
        if (!id) {
            atomic_clear(&s->busy);
            return EAF_IO;
        }
#ifdef CONFIG_SCHED_CPU_MASK
        if (options->cpu >= 0 && k_thread_cpu_pin(id, options->cpu)) {
            k_thread_abort(id);
            atomic_clear(&s->busy);
            return EAF_IO;
        }
#endif
        thread->impl = s;
        k_thread_start(id);
        return EAF_OK;
    }
    return EAF_IO;
}
int hal_thread_join(eaf_thread_t *thread) {
    if (!thread || !thread->impl)
        return EAF_INVALID;
    thread_slot *s = thread->impl;
    if (k_thread_join(&s->thread, K_FOREVER))
        return EAF_IO;
    thread->impl = NULL;
    atomic_clear(&s->busy);
    return EAF_OK;
}
int hal_sem_init(eaf_sem_t *sem) {
    if (!sem || sem->impl)
        return EAF_INVALID;
    for (size_t i = 0; i < CONFIG_EAF_SEM_COUNT; ++i) {
        sem_slot *s = &sems[i];
        if (!atomic_cas(&s->busy, 0, 1))
            continue;
        if (k_sem_init(&s->sem, 0, 1)) {
            atomic_clear(&s->busy);
            return EAF_IO;
        }
        sem->impl = s;
        return EAF_OK;
    }
    return EAF_IO;
}
int hal_sem_take(eaf_sem_t *sem, uint32_t timeout_ms) {
    if (!sem || !sem->impl)
        return EAF_INVALID;
    sem_slot *s = sem->impl;
    int rc = k_sem_take(&s->sem, K_MSEC(timeout_ms));
    return !rc ? EAF_OK : ((rc == -EAGAIN || rc == -EBUSY) ? EAF_TIMEOUT : EAF_IO);
}
void hal_sem_give(eaf_sem_t *sem) {
    if (sem && sem->impl) {
        sem_slot *s = sem->impl;
        k_sem_give(&s->sem);
    }
}
void hal_sem_deinit(eaf_sem_t *sem) {
    if (!sem || !sem->impl)
        return;
    sem_slot *s = sem->impl;
    sem->impl = NULL;
    atomic_clear(&s->busy);
}
uint64_t hal_monotonic_time_us(void) {
    return k_ticks_to_us_floor64((uint64_t)k_uptime_ticks());
}
int hal_sleep_until_us(uint64_t deadline) {
    uint64_t now;
    while ((now = hal_monotonic_time_us()) < deadline) {
        uint64_t left = deadline - now;
        /* Bound conversion to the signed timeout domain. */
        if (left > UINT32_MAX)
            left = UINT32_MAX;
        (void)k_sleep(K_USEC(left));
    }
    return EAF_OK;
}
void hal_sleep_ms(uint32_t ms) {
    (void)k_sleep(K_MSEC(ms));
}
