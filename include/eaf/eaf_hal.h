#pragma once
#include <eaf/eaf_types.h>
#include <stdatomic.h>
_Static_assert(sizeof(unsigned int) == sizeof(uint32_t), "32-bit unsigned required");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Always lock-free atomics required");
typedef struct {
    atomic_uint value;
} eaf_atomic_u32_t;
void hal_atomic_set(eaf_atomic_u32_t *a, uint32_t value);
uint32_t hal_atomic_get(const eaf_atomic_u32_t *a);
typedef struct {
    void *impl;
} eaf_thread_t;
typedef struct {
    void *impl;
} eaf_sem_t;
typedef enum { EAF_THREAD_DECODER, EAF_THREAD_AUDIO } eaf_thread_role_t;
typedef struct {
    eaf_thread_role_t role;
    int cpu;       /* -1: unrestricted; otherwise an OS CPU index. */
    bool realtime; /* Linux: request FIFO scheduling; errors never silently downgrade.
                      Zephyr: both modes use role-specific preemptive priorities. */
} eaf_thread_options_t;
/* Options are copied during create. Entry cannot run before scheduling/affinity
   setup succeeds. Linux default is ordinary scheduling; Zephyr audio outranks decoder. */
int hal_thread_create_with_options(eaf_thread_t *thread, void (*entry)(void *), void *arg,
                                   const eaf_thread_options_t *options);
/* Init/create may allocate; join and deinit only after audio stops. */
int hal_thread_create(eaf_thread_t *thread, void (*entry)(void *), void *arg);
int hal_thread_join(eaf_thread_t *thread);
int hal_sem_init(eaf_sem_t *sem);
/* Binary, coalescing wake hint: recheck the caller's atomic predicate after wake.
   Relative monotonic timeout; zero polls. EAF_TIMEOUT differs from EAF_IO.
   No concurrent init/deinit with give/take, and no deinit while waiters exist. */
int hal_sem_take(eaf_sem_t *sem, uint32_t timeout_ms);
void hal_sem_give(eaf_sem_t *sem);
void hal_sem_deinit(eaf_sem_t *sem);
uint64_t hal_monotonic_time_us(void);
void hal_sleep_ms(uint32_t ms);
int hal_sleep_until_us(uint64_t deadline);
typedef struct eaf_sink eaf_sink_t;
struct eaf_sink_ops {
    int (*init)(eaf_sink_t *, const eaf_format_t *, size_t);
    int (*start)(eaf_sink_t *);
    /* Immediate DROP, including any acquired buffer. Failure retains ownership. */
    int (*stop)(eaf_sink_t *);
    int (*acquire_buf)(eaf_sink_t *, eaf_buffer_t **);
    /* EOS must finish a bounded driver-queue drain before success. On error the
       owner must stop; it must not retry processing a partially written block. */
    int (*commit_buf)(eaf_sink_t *, eaf_buffer_t *);
    int (*adjust_ppm)(eaf_sink_t *, int32_t);
    /* Deinit also handles partial init/start. On failure preserve a retry-safe
       context; callers must not assume all resources were released. */
    int (*deinit)(eaf_sink_t *);
};
struct eaf_sink {
    const struct eaf_sink_ops *ops;
    void *driver_data;
};
