#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bounded compressed-input ring shared by callback-style decoders (dr_flac,
   dr_mp3). A producer pushes bytes and the decode owner reads them. When the
   ring is empty and the stream is not at EOF, `read` calls `await` (which may
   block on the decode owner) until data arrives or the wait fails; a short read
   therefore means end-of-stream to the decoder. Storage is caller-owned. */

/* Return 0 once more data may have arrived, non-zero to abort the read. */
typedef int (*eaf_dec_await_fn)(void *ctx);

typedef struct {
    uint8_t *ring;
    size_t capacity;
    size_t tail;
    size_t count;
    bool eof;
    eaf_dec_await_fn await;
    void *await_ctx;
} eaf_dec_input_t;

void eaf_dec_input_init(eaf_dec_input_t *input, uint8_t *ring, size_t capacity,
                        eaf_dec_await_fn await, void *await_ctx);
void eaf_dec_input_reset(eaf_dec_input_t *input);
/* Mark end of input so a trailing partial buffer can be consumed. The caller
   must also release any waiter blocked in `await`. */
void eaf_dec_input_finish(eaf_dec_input_t *input);
/* Copy accepted bytes; returns the count (may be less than length). */
size_t eaf_dec_input_push(eaf_dec_input_t *input, const uint8_t *data, size_t length);
/* Fill up to length bytes; returns the count read. May block via await. */
size_t eaf_dec_input_read(eaf_dec_input_t *input, void *out, size_t length);
