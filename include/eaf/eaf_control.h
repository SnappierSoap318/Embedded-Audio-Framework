#pragma once
#include <eaf/eaf_hal.h>
#define EAF_CONTROL_CAPACITY 16u
typedef enum { EAF_COMMAND_STOP, EAF_COMMAND_SEEK, EAF_COMMAND_GAIN } eaf_command_type_t;
typedef struct {
    eaf_command_type_t type;
    uint64_t frame;
    int32_t gain;
} eaf_command_t;
typedef struct {
    eaf_command_t slots[EAF_CONTROL_CAPACITY];
    eaf_atomic_u32_t read_cursor, write_cursor;
} eaf_control_queue_t;
/* Exactly one control producer and one audio consumer; reset only when idle. */
void eaf_control_init(eaf_control_queue_t *q);
bool eaf_control_push(eaf_control_queue_t *q, const eaf_command_t *command);
bool eaf_control_pop(eaf_control_queue_t *q, eaf_command_t *command);
