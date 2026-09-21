#pragma once
#include <eaf/eaf_decoder.h>
#include <eaf/eaf_sendspin.h>
#include <eaf/eaf_sync.h>
#include <eaf/eaf_types.h>

/* Portable Sendspin producer: turns interleaved PCM16/24/32 or compressed
   frames (FLAC/Opus/MP3/Vorbis via an injected adapter) into stereo Q1.31,
   applies hard sync (drop late audio) using the shared time filter, and hands
   frames to a caller-provided sink (e.g. the board output owner). */

#define EAF_SENDPIN_PLAYER_CHUNK_FRAMES 128u

typedef uint32_t (*eaf_sendspin_sink_fn)(void *ctx, const int32_t *samples, uint32_t frames);

/* Caller-owned decoder binding for compressed streams. The adapter is chosen by
   the platform from the negotiated codec; `scratch` must be large enough for
   the adapter's largest decoded frame (Vorbis needs a full Ogg frame). Only the
   storage is caller-owned; the player owns one worker per active stream. */
typedef struct {
    const eaf_decoder_ops_t *ops;
    void *ctx;
    int32_t *scratch;
    uint32_t scratch_frames;
} eaf_sendspin_decoder_t;

typedef struct {
    const eaf_sendspin_time_filter_t *filter;
    eaf_sendspin_sink_fn sink;
    void *sink_ctx;
    eaf_format_t format;
    uint8_t input_channels, input_bits;
    bool active, synchronized, drop_late, compressed;
    int64_t last_latency_us;
    uint32_t frames_written, frames_dropped, chunks;
    /* Compressed path: the injected adapter binding and the player-owned worker
       that drives it. Unused (and ignored) for PCM streams. */
    eaf_sendspin_decoder_t decoder;
    eaf_decode_worker_t worker;
    double active_ratio;
    int32_t resample_scratch[(EAF_SENDPIN_PLAYER_CHUNK_FRAMES + 2u) * 2u];
    /* Multiroom drift control: hold presentation latency at a target by
       resampling what is written into the sink. Disabled by default. */
    bool rate_control, rate_auto_target;
    double target_latency_us, resample_credit, rate_calibration_sum;
    uint32_t rate_calibration;
    int32_t rate_ppm;
    int64_t rate_update_us;
    eaf_sync_controller_t controller;
} eaf_sendspin_player_t;

void eaf_sendspin_player_init(eaf_sendspin_player_t *player, eaf_sendspin_sink_fn sink,
                              void *sink_ctx);
/* Bind the compressed-stream decoder adapter (storage stays caller-owned). Must
   be called before playing a compressed stream; PCM needs none. */
void eaf_sendspin_player_set_decoder(eaf_sendspin_player_t *player,
                                     const eaf_sendspin_decoder_t *decoder);
int eaf_sendspin_player_begin(eaf_sendspin_player_t *player,
                              const eaf_sendspin_time_filter_t *filter,
                              const eaf_sendspin_stream_start_t *start);
/* Enable multiroom drift control. The controller holds the measured
   presentation latency by resampling the PCM written to the sink; call before
   playback. A positive target_latency_ms fixes the target; zero or negative
   latches it to the average of the first converging measurements, which avoids
   fighting the server's own scheduling. Disabled until called. */
void eaf_sendspin_player_set_rate_control(eaf_sendspin_player_t *player, double target_latency_ms);
/* Converts PCM16 and hands stereo Q1.31 to the sink; the sink's unaccepted
   remainder is counted as dropped. Audio whose scheduled client play time has
   already passed is dropped without conversion. */
int eaf_sendspin_player_write(eaf_sendspin_player_t *player, int64_t server_timestamp_us,
                              const uint8_t *pcm, size_t length);
void eaf_sendspin_player_finish(eaf_sendspin_player_t *player);
