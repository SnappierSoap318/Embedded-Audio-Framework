#pragma once
#include <eaf/eaf_sendspin.h>
#include <eaf/eaf_types.h>

/* Portable Sendspin producer: converts interleaved PCM16/24/32 chunks to stereo
   Q1.31, applies hard sync (drop late audio) using the shared time filter, and
   hands frames to a caller-provided sink (e.g. the board output owner). */

#define EAF_SENDPIN_PLAYER_CHUNK_FRAMES 128u

typedef uint32_t (*eaf_sendspin_sink_fn)(void *ctx, const int32_t *samples, uint32_t frames);

typedef struct {
    const eaf_sendspin_time_filter_t *filter;
    eaf_sendspin_sink_fn sink;
    void *sink_ctx;
    eaf_format_t format;
    uint8_t input_channels, input_bits;
    bool active, synchronized, drop_late;
    int64_t last_latency_us;
    uint32_t frames_written, frames_dropped, chunks;
} eaf_sendspin_player_t;

void eaf_sendspin_player_init(eaf_sendspin_player_t *player, eaf_sendspin_sink_fn sink,
                              void *sink_ctx);
int eaf_sendspin_player_begin(eaf_sendspin_player_t *player,
                              const eaf_sendspin_time_filter_t *filter,
                              const eaf_sendspin_stream_start_t *start);
/* Converts PCM16 and hands stereo Q1.31 to the sink; the sink's unaccepted
   remainder is counted as dropped. Audio whose scheduled client play time has
   already passed is dropped without conversion. */
int eaf_sendspin_player_write(eaf_sendspin_player_t *player, int64_t server_timestamp_us,
                              const uint8_t *pcm, size_t length);
void eaf_sendspin_player_finish(eaf_sendspin_player_t *player);
