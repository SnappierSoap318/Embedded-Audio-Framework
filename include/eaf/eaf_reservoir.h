#pragma once
#include <eaf/eaf_hal.h>
typedef enum {
    EAF_RESERVOIR_PREBUFFERING,
    EAF_RESERVOIR_STREAMING,
    EAF_RESERVOIR_UNDERRUN,
    EAF_RESERVOIR_DRAINED
} eaf_reservoir_state_t;
typedef struct {
    int32_t *storage;
    uint32_t capacity, high_watermark, backpressure_low, backpressure_high;
    eaf_format_t format;
    eaf_atomic_u32_t read_cursor, write_cursor;
    eaf_atomic_u32_t finished;
    /* Remaining state belongs exclusively to the consumer. */
    eaf_reservoir_state_t state;
    uint32_t underruns;
    uint64_t frames_read;
    int32_t last[EAF_MAX_CHANNELS];
    void (*wake_producer)(void *);
    void *wake_ctx;
} eaf_reservoir_t;
int eaf_reservoir_init(eaf_reservoir_t *r, int32_t *storage, uint32_t capacity, eaf_format_t fmt,
                       uint32_t high_watermark);
/* Reset only while both endpoints are quiescent. */
void eaf_reservoir_reset(eaf_reservoir_t *r);
/* Partial writes are allowed. Caller retains unwritten frames. */
uint32_t eaf_reservoir_write(eaf_reservoir_t *r, const int32_t *src, uint32_t frames);
/* Producer publishes EOF after its final successful write; no more writes until reset. */
void eaf_reservoir_finish(eaf_reservoir_t *r);
uint32_t eaf_reservoir_level(const eaf_reservoir_t *r);
/* Producer side predicate; resume below backpressure_low. */
bool eaf_reservoir_backpressure(const eaf_reservoir_t *r);
/* Always fills the requested block, including recovery silence. */
int eaf_reservoir_pull(eaf_reservoir_t *r, eaf_buffer_t *buf);
