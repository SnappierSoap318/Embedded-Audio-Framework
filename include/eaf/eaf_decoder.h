#pragma once
#include <eaf/eaf_types.h>

/* Decoder adapter contract for compressed and PCM source streams.
 *
 * Adapters are pure algorithmic state machines: they never touch the OS, the
 * network, the reservoir or a sink. A single owner (a decode worker) drives one
 * adapter. `open` may allocate off the audio path and `close` releases after the
 * stream stops; `push`, `pull` and `reset` must not block or allocate.
 *
 * All output is interleaved signed 32-bit fixed point (Q1.31). */

typedef enum {
    EAF_CODEC_NONE = 0,
    EAF_CODEC_PCM,
    EAF_CODEC_FLAC,
    EAF_CODEC_OPUS,
    EAF_CODEC_MP3,
    EAF_CODEC_VORBIS
} eaf_codec_t;

typedef struct {
    eaf_codec_t codec;
    uint32_t sample_rate; /* stream rate from the container */
    uint8_t channels;     /* container channel count */
    uint8_t bit_depth;    /* EAF_CODEC_PCM only: 16, 24 or 32 */
    /* Codec configuration block (FLAC STREAMINFO, Opus header, ...). Points at
       caller-owned wire memory that must stay valid until `open` returns. */
    const uint8_t *extra;
    size_t extra_length;
} eaf_decoder_config_t;

typedef struct {
    /* Allocate and bind state for cfg. `output` is the requested sink format.
       `pull` reports the format it actually produced and must match `output`
       (rate, channel count and mask) for the owner to accept it. */
    int (*open)(void *ctx, const eaf_decoder_config_t *cfg, const eaf_format_t *output);
    /* Feed compressed bytes. Sets *consumed to the bytes accepted, which may be
       zero when the adapter's input buffer is full. Does not block. */
    int (*push)(void *ctx, const uint8_t *data, size_t length, size_t *consumed);
    /* Emit up to max_frames interleaved Q1.31 frames and report the count and
       format. *frames may be zero when more input is required. */
    int (*pull)(void *ctx, int32_t *pcm, uint32_t max_frames, uint32_t *frames,
                eaf_format_t *format);
    /* Drop codec history and buffered input after a discontinuity. */
    int (*reset)(void *ctx);
    /* Release any allocation made by open; safe after a partial open. */
    void (*close)(void *ctx);
} eaf_decoder_ops_t;

typedef struct {
    const eaf_decoder_ops_t *ops;
    void *ctx;
} eaf_decoder_t;

/* Contract enforcement shared by every caller. `open` additionally validates
   cfg and output; the others reject unopened/malformed adapters. */
int eaf_decoder_open(eaf_decoder_t *decoder, const eaf_decoder_config_t *cfg,
                     const eaf_format_t *output);
int eaf_decoder_push(eaf_decoder_t *decoder, const uint8_t *data, size_t length, size_t *consumed);
int eaf_decoder_pull(eaf_decoder_t *decoder, int32_t *pcm, uint32_t max_frames, uint32_t *frames,
                     eaf_format_t *format);
int eaf_decoder_reset(eaf_decoder_t *decoder);
void eaf_decoder_close(eaf_decoder_t *decoder);

/* Sink callback for decoded Q1.31 frames, matching eaf_board_output_write. */
typedef uint32_t (*eaf_pcm_write_fn)(void *ctx, const int32_t *samples, uint32_t frames);

/* Bridges one decoder adapter to a PCM sink, retaining a partially accepted
   buffer across steps so no decoded frame is lost or written twice. The worker
   owns the producer endpoint; a single worker drives each decoder. */
typedef struct {
    eaf_decoder_t decoder;
    eaf_format_t format;
    eaf_pcm_write_fn sink;
    void *sink_ctx;
    int32_t *scratch;
    uint32_t scratch_frames;
    uint32_t pending;
    uint32_t sent;
    uint32_t frames_written;
    uint32_t frames_dropped;
    bool open;
} eaf_decode_worker_t;

/* scratch must hold scratch_frames * output->num_channels samples. */
int eaf_decode_worker_init(eaf_decode_worker_t *worker, const eaf_decoder_t *decoder,
                           eaf_pcm_write_fn sink, void *sink_ctx, int32_t *scratch,
                           uint32_t scratch_frames, const eaf_format_t *output);
int eaf_decode_worker_open(eaf_decode_worker_t *worker, const eaf_decoder_config_t *cfg);
/* Returns true while the worker may accept more compressed input; false when a
   decoded buffer is still waiting on sink backpressure. */
bool eaf_decode_worker_can_push(const eaf_decode_worker_t *worker);
int eaf_decode_worker_push(eaf_decode_worker_t *worker, const uint8_t *data, size_t length,
                           size_t *consumed);
/* Drain decoded frames into the sink and pull at most one buffer. Retains a
   partial write. Returns EAF_OK on progress, or a decoder error. */
int eaf_decode_worker_step(eaf_decode_worker_t *worker);
int eaf_decode_worker_reset(eaf_decode_worker_t *worker);
void eaf_decode_worker_close(eaf_decode_worker_t *worker);
