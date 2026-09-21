#include <eaf/eaf_dec_input.h>
#include <string.h>

void eaf_dec_input_init(eaf_dec_input_t *input, uint8_t *ring, size_t capacity,
                        eaf_dec_await_fn await, void *await_ctx) {
    if (!input)
        return;
    *input = (eaf_dec_input_t){
        .ring = ring, .capacity = capacity, .await = await, .await_ctx = await_ctx};
}

void eaf_dec_input_reset(eaf_dec_input_t *input) {
    if (!input)
        return;
    input->tail = 0;
    input->count = 0;
    input->eof = false;
}

void eaf_dec_input_finish(eaf_dec_input_t *input) {
    if (input)
        input->eof = true;
}

size_t eaf_dec_input_push(eaf_dec_input_t *input, const uint8_t *data, size_t length) {
    if (!input || !input->ring || input->eof || length == 0u)
        return 0;
    size_t space = input->capacity - input->count;
    size_t take = length < space ? length : space;
    size_t write = (input->tail + input->count) % input->capacity;
    for (size_t i = 0; i < take; ++i) {
        input->ring[write] = data[i];
        write = (write + 1u) % input->capacity;
    }
    input->count += take;
    return take;
}

size_t eaf_dec_input_read(eaf_dec_input_t *input, void *out, size_t length) {
    if (!input)
        return 0;
    uint8_t *dst = out;
    size_t got = 0;
    while (got < length) {
        if (input->count == 0u) {
            if (input->eof || input->await == NULL)
                break;
            if (input->await(input->await_ctx) != 0)
                break;
            continue;
        }
        size_t chunk = input->capacity - input->tail;
        if (chunk > input->count)
            chunk = input->count;
        if (chunk > length - got)
            chunk = length - got;
        memcpy(dst + got, input->ring + input->tail, chunk);
        input->tail = (input->tail + chunk) % input->capacity;
        input->count -= chunk;
        got += chunk;
    }
    return got;
}
