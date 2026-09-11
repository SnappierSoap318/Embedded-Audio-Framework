#include <eaf/eaf_control.h>
void eaf_control_init(eaf_control_queue_t *q) {
    atomic_init(&q->read_cursor.value, 0u);
    atomic_init(&q->write_cursor.value, 0u);
}
bool eaf_control_push(eaf_control_queue_t *q, const eaf_command_t *command) {
    if (!q || !command)
        return false;
    uint32_t write = hal_atomic_get(&q->write_cursor);
    if (write - hal_atomic_get(&q->read_cursor) == EAF_CONTROL_CAPACITY)
        return false;
    q->slots[write % EAF_CONTROL_CAPACITY] = *command;
    hal_atomic_set(&q->write_cursor, write + 1u);
    return true;
}
bool eaf_control_pop(eaf_control_queue_t *q, eaf_command_t *command) {
    if (!q || !command)
        return false;
    uint32_t read = hal_atomic_get(&q->read_cursor);
    if (read == hal_atomic_get(&q->write_cursor))
        return false;
    *command = q->slots[read % EAF_CONTROL_CAPACITY];
    hal_atomic_set(&q->read_cursor, read + 1u);
    return true;
}
