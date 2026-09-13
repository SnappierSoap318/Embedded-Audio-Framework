#pragma once
#include <eaf/eaf_reservoir.h>
#include <eaf/eaf_sendspin.h>

/* Portable Sendspin producer: converts interleaved PCM16 chunks into the
   framework's Q1.31 stereo reservoir and applies hard sync (drop late audio)
   using the shared time filter. The consumer/sink/worker stays platform-side. */

#define EAF_SENDPIN_PLAYER_CHUNK_FRAMES 128u

typedef struct {
    eaf_reservoir_t reservoir;
    const eaf_sendspin_time_filter_t *filter;
    int32_t *storage;
    uint32_t capacity_frames;
    eaf_format_t format;
    uint8_t input_channels;
    bool active, synchronized;
    int64_t drop_ahead_us;
    int64_t last_latency_us;
    uint32_t frames_written, frames_dropped, chunks;
} eaf_sendspin_player_t;

/* storage must hold capacity_frames * 2 Q1.31 samples; capacity is a power of two. */
void eaf_sendspin_player_init(eaf_sendspin_player_t *player, int32_t *storage,
                              uint32_t capacity_frames);
int eaf_sendspin_player_begin(eaf_sendspin_player_t *player,
                              const eaf_sendspin_time_filter_t *filter,
                              const eaf_sendspin_stream_start_t *start);
/* Converts PCM16 and writes as much as the reservoir accepts; the remainder is
   counted as dropped. Audio whose scheduled client play time has already passed
   is dropped without conversion. */
int eaf_sendspin_player_write(eaf_sendspin_player_t *player, int64_t server_timestamp_us,
                              const uint8_t *pcm, size_t length);
void eaf_sendspin_player_finish(eaf_sendspin_player_t *player);

uint32_t eaf_sendspin_player_capacity_ms(const eaf_sendspin_player_t *player);
uint32_t eaf_sendspin_player_buffer_bytes(const eaf_sendspin_player_t *player);
