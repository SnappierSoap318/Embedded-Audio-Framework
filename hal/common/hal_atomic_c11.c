#include <eaf/eaf_hal.h>
void hal_atomic_set(eaf_atomic_u32_t *a, uint32_t value) {
    atomic_store_explicit(&a->value, value, memory_order_release);
}
uint32_t hal_atomic_get(const eaf_atomic_u32_t *a) {
    return atomic_load_explicit(&a->value, memory_order_acquire);
}
