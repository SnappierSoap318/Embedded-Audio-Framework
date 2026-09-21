#include <eaf/eaf_hal.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <stdatomic.h>

/* FreeRTOS ranks a larger numeric priority higher, so audio must be the larger
   value here; Zephyr's adapter orders them the other way. */
_Static_assert(CONFIG_EAF_AUDIO_PRIORITY > CONFIG_EAF_DECODER_PRIORITY,
               "Audio must outrank the decoder");
_Static_assert(CONFIG_EAF_DECODER_PRIORITY > tskIDLE_PRIORITY,
               "EAF worker priority must exceed the idle task");
_Static_assert(CONFIG_EAF_AUDIO_PRIORITY < configMAX_PRIORITIES,
               "EAF worker priority must exist in the FreeRTOS configuration");

/* Statically backed pools; exhaustion is explicit, never falls back to heap. */
typedef struct {
    StaticTask_t task;
    StackType_t stack[CONFIG_EAF_THREAD_STACK_SIZE / sizeof(StackType_t)];
    StaticSemaphore_t done_storage;
    SemaphoreHandle_t done;
    atomic_uint busy;
    void (*entry)(void *);
    void *arg;
} thread_slot;

typedef struct {
    StaticSemaphore_t storage;
    SemaphoreHandle_t sem;
    atomic_uint busy;
} sem_slot;

static thread_slot threads[CONFIG_EAF_THREAD_COUNT];
static sem_slot sems[CONFIG_EAF_SEM_COUNT];

static void entry_bridge(void *arg) {
    thread_slot *slot = arg;
    slot->entry(slot->arg);
    xSemaphoreGive(slot->done);
    vTaskDelete(NULL);
}

int hal_thread_create(eaf_thread_t *thread, void (*entry)(void *), void *arg) {
    const eaf_thread_options_t options = {EAF_THREAD_DECODER, -1, false};
    return hal_thread_create_with_options(thread, entry, arg, &options);
}

int hal_thread_create_with_options(eaf_thread_t *thread, void (*entry)(void *), void *arg,
                                   const eaf_thread_options_t *options) {
    if (!thread || thread->impl || !entry || !options ||
        (options->role != EAF_THREAD_DECODER && options->role != EAF_THREAD_AUDIO) ||
        options->cpu < -1 || options->cpu >= configNUMBER_OF_CORES)
        return EAF_INVALID;
    UBaseType_t priority =
        options->role == EAF_THREAD_AUDIO ? CONFIG_EAF_AUDIO_PRIORITY : CONFIG_EAF_DECODER_PRIORITY;
    BaseType_t core = options->cpu < 0 ? tskNO_AFFINITY : (BaseType_t)options->cpu;
    for (size_t i = 0; i < CONFIG_EAF_THREAD_COUNT; ++i) {
        thread_slot *s = &threads[i];
        unsigned expected = 0u;
        if (!atomic_compare_exchange_strong(&s->busy, &expected, 1u))
            continue;
        if (!s->done)
            s->done = xSemaphoreCreateBinaryStatic(&s->done_storage);
        if (!s->done) {
            atomic_store(&s->busy, 0u);
            return EAF_IO;
        }
        s->entry = entry;
        s->arg = arg;
        TaskHandle_t handle = xTaskCreateStaticPinnedToCore(
            entry_bridge, "eaf", (uint32_t)(sizeof(s->stack) / sizeof(s->stack[0])), s, priority,
            s->stack, &s->task, core);
        if (!handle) {
            atomic_store(&s->busy, 0u);
            return EAF_IO;
        }
        thread->impl = s;
        return EAF_OK;
    }
    return EAF_IO;
}

int hal_thread_join(eaf_thread_t *thread) {
    if (!thread || !thread->impl)
        return EAF_INVALID;
    thread_slot *s = thread->impl;
    if (xSemaphoreTake(s->done, portMAX_DELAY) != pdTRUE)
        return EAF_IO;
    thread->impl = NULL;
    atomic_store(&s->busy, 0u);
    return EAF_OK;
}

int hal_sem_init(eaf_sem_t *sem) {
    if (!sem || sem->impl)
        return EAF_INVALID;
    for (size_t i = 0; i < CONFIG_EAF_SEM_COUNT; ++i) {
        sem_slot *s = &sems[i];
        unsigned expected = 0u;
        if (!atomic_compare_exchange_strong(&s->busy, &expected, 1u))
            continue;
        s->sem = xSemaphoreCreateBinaryStatic(&s->storage);
        if (!s->sem) {
            atomic_store(&s->busy, 0u);
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
    return xSemaphoreTake(s->sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE ? EAF_OK : EAF_TIMEOUT;
}

void hal_sem_give(eaf_sem_t *sem) {
    if (sem && sem->impl) {
        sem_slot *s = sem->impl;
        (void)xSemaphoreGive(s->sem);
    }
}

void hal_sem_deinit(eaf_sem_t *sem) {
    if (!sem || !sem->impl)
        return;
    sem_slot *s = sem->impl;
    sem->impl = NULL;
    atomic_store(&s->busy, 0u);
}

uint64_t hal_monotonic_time_us(void) {
    return (uint64_t)esp_timer_get_time();
}

int hal_sleep_until_us(uint64_t deadline) {
    for (;;) {
        uint64_t now = hal_monotonic_time_us();
        if (now >= deadline)
            return EAF_OK;
        uint64_t left = deadline - now;
        /* Bound the conversion to the 32-bit tick domain. */
        if (left > UINT32_MAX)
            left = UINT32_MAX;
        TickType_t ticks = (TickType_t)((left + (1000000u / configTICK_RATE_HZ) - 1u) /
                                        (1000000u / configTICK_RATE_HZ));
        if (ticks > 0)
            vTaskDelay(ticks);
        else
            esp_rom_delay_us((uint32_t)left);
    }
}

void hal_sleep_ms(uint32_t ms) {
    if (!ms)
        return;
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}
