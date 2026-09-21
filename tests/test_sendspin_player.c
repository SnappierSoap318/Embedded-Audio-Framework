#include "check.h"
#include <eaf/eaf_dec_flac.h>
#include <eaf/eaf_dec_vorbis.h>
#include <eaf/eaf_hal.h>
#include <eaf/eaf_sendspin_player.h>
#include <stdio.h>
#include <string.h>

static eaf_sendspin_player_t player;
static int32_t recorded[8192u * 2u];
static uint32_t recorded_frames;
static uint32_t sink_limit = UINT32_MAX;
static uint64_t now;
static eaf_sendspin_time_filter_t filter;

uint64_t __wrap_hal_monotonic_time_us(void) {
    return now;
}

static uint32_t counted;
static uint32_t count_sink(void *ctx, const int32_t *samples, uint32_t frames) {
    (void)ctx;
    (void)samples;
    counted += frames;
    return frames;
}

static uint32_t record(void *ctx, const int32_t *samples, uint32_t frames) {
    (void)ctx;
    if (frames > sink_limit)
        frames = sink_limit;
    for (size_t i = 0; i < (size_t)frames * 2u; ++i)
        recorded[(size_t)recorded_frames * 2u + i] = samples[i];
    recorded_frames += frames;
    return frames;
}

static void put_le16(uint8_t *dst, int16_t value) {
    uint16_t raw = (uint16_t)value;
    dst[0] = (uint8_t)raw;
    dst[1] = (uint8_t)(raw >> 8);
}

static void put_le24(uint8_t *dst, int32_t value) {
    uint32_t raw = (uint32_t)value & 0xFFFFFFu;
    dst[0] = (uint8_t)raw;
    dst[1] = (uint8_t)(raw >> 8);
    dst[2] = (uint8_t)(raw >> 16);
}

static void put_le32(uint8_t *dst, int32_t value) {
    uint32_t raw = (uint32_t)value;
    dst[0] = (uint8_t)raw;
    dst[1] = (uint8_t)(raw >> 8);
    dst[2] = (uint8_t)(raw >> 16);
    dst[3] = (uint8_t)(raw >> 24);
}

static eaf_sendspin_stream_start_t make_start(uint8_t channels) {
    return (eaf_sendspin_stream_start_t){.codec = EAF_SENDPIN_CODEC_PCM,
                                         .sample_rate = 44100,
                                         .channels = channels,
                                         .bit_depth = 16};
}

/* A FLAC stream is framed by the server and decoded through the injected
   adapter; the fixture is a 4096-frame 16-bit ramp, so the expected Q1.31 output
   is exact. */
static void compressed_stream_test(const char *path) {
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    static uint8_t flac[65536];
    size_t length = fread(flac, 1, sizeof(flac), file);
    fclose(file);
    CHECK(length > 0);

    static eaf_dec_flac_t flac_state;
    static int32_t flac_scratch[1024u * 2u];
    eaf_dec_flac_configure(&flac_state, NULL, NULL);
    eaf_sendspin_decoder_t decoder = {.ops = &eaf_dec_flac_ops,
                                      .ctx = &flac_state,
                                      .scratch = flac_scratch,
                                      .scratch_frames = 1024u};

    recorded_frames = 0;
    eaf_sendspin_player_init(&player, record, NULL);
    eaf_sendspin_player_set_decoder(&player, &decoder);
    eaf_sendspin_stream_start_t start = {
        .codec = EAF_SENDPIN_CODEC_FLAC, .sample_rate = 44100, .channels = 2, .bit_depth = 16};
    CHECK(!eaf_sendspin_player_begin(&player, &filter, &start));
    CHECK(player.compressed);

    int64_t future = eaf_sendspin_compute_server_time(&filter, (int64_t)now + 1000000);
    /* dr_flac latches EOF on a short read, so the whole stream is staged before
       draining (a live platform instead supplies an await hook on a decode
       thread; see TASKS C7). */
    CHECK(!eaf_sendspin_player_write(&player, future, flac, length));
    CHECK(player.frames_written == 4096u && player.frames_dropped == 0u);
    CHECK(recorded_frames == 4096u);
    for (uint32_t i = 0; i < 4096u; ++i) {
        CHECK(recorded[(size_t)i * 2u] == eaf_pcm16_to_q31((int16_t)(i * 101)));
        CHECK(recorded[(size_t)i * 2u + 1u] == eaf_pcm16_to_q31((int16_t)(-i * 101)));
    }
    eaf_sendspin_player_finish(&player);
    CHECK(!player.active && !player.compressed);

    /* A compressed stream with no bound decoder is refused rather than begun. */
    eaf_sendspin_player_init(&player, record, NULL);
    CHECK(eaf_sendspin_player_begin(&player, &filter, &start) == EAF_UNSUPPORTED);
    CHECK(!player.active);
}

/* Vorbis arrives across many writes, so this exercises incremental feeding: the
   decoder must make progress as bytes land, not only once the stream ends. */
static void incremental_stream_test(const char *path) {
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    static uint8_t ogg[65536];
    size_t length = fread(ogg, 1, sizeof(ogg), file);
    fclose(file);
    CHECK(length > 0);

    static eaf_dec_vorbis_t vorbis_state;
    static int32_t vorbis_scratch[4096u * 2u];
    eaf_sendspin_decoder_t decoder = {.ops = &eaf_dec_vorbis_ops,
                                      .ctx = &vorbis_state,
                                      .scratch = vorbis_scratch,
                                      .scratch_frames = 4096u};

    recorded_frames = 0;
    eaf_sendspin_player_init(&player, record, NULL);
    eaf_sendspin_player_set_decoder(&player, &decoder);
    eaf_sendspin_stream_start_t start = {
        .codec = EAF_SENDPIN_CODEC_VORBIS, .sample_rate = 44100, .channels = 2, .bit_depth = 16};
    CHECK(!eaf_sendspin_player_begin(&player, &filter, &start));
    CHECK(player.compressed);

    int64_t future = eaf_sendspin_compute_server_time(&filter, (int64_t)now + 1000000);
    for (size_t offset = 0; offset < length; offset += 128u) {
        size_t chunk = length - offset;
        if (chunk > 128u)
            chunk = 128u;
        CHECK(!eaf_sendspin_player_write(&player, future, ogg + offset, chunk));
    }
    /* Lossy and delay-padded, but the tone's frames must all reach the sink. */
    CHECK(player.frames_written > 3000u && player.frames_written < 5000u);
    CHECK(player.frames_dropped == 0u && recorded_frames == player.frames_written);
    double energy = 0.0;
    for (uint32_t i = 0; i < recorded_frames; ++i) {
        double sample = (double)(recorded[(size_t)i * 2u] >> 16);
        energy += sample * sample;
    }
    CHECK(energy > 0.0);
    eaf_sendspin_player_finish(&player);
}

int main(int argc, char **argv) {
    CHECK(argc >= 3);
    eaf_sendspin_time_filter_init(&filter);
    eaf_sendspin_time_filter_update(&filter, 0, 100, 1000000u);
    eaf_sendspin_time_filter_update(&filter, 0, 100, 2000000u);
    CHECK(eaf_sendspin_time_synchronized(&filter));
    now = 5000000u;

    eaf_sendspin_player_init(&player, record, NULL);
    eaf_sendspin_stream_start_t start = make_start(2);
    CHECK(!eaf_sendspin_player_begin(&player, &filter, &start));
    CHECK(eaf_sendspin_player_begin(&player, &filter, &start) == EAF_STATE); /* already active */

    uint8_t pcm[8];
    put_le16(pcm + 0, 1000);
    put_le16(pcm + 2, -1000);
    put_le16(pcm + 4, 2000);
    put_le16(pcm + 6, -2000);
    int64_t future = eaf_sendspin_compute_server_time(&filter, (int64_t)now + 1000000);
    CHECK(!eaf_sendspin_player_write(&player, future, pcm, sizeof(pcm)));
    CHECK(player.frames_written == 2 && player.frames_dropped == 0 && player.chunks == 1);
    CHECK(recorded_frames == 2);
    CHECK(recorded[0] == 1000 * 65536 && recorded[1] == -1000 * 65536);
    CHECK(recorded[2] == 2000 * 65536 && recorded[3] == -2000 * 65536);

    /* Scheduled in the past: dropped only when hard-sync drop is enabled. */
    player.drop_late = true;
    int64_t past = eaf_sendspin_compute_server_time(&filter, (int64_t)now - 1000000);
    CHECK(!eaf_sendspin_player_write(&player, past, pcm, sizeof(pcm)));
    CHECK(player.frames_written == 2 && player.frames_dropped == 2 && player.chunks == 2);
    CHECK(recorded_frames == 2);
    /* With drop disabled the late chunk is still written. */
    player.drop_late = false;
    CHECK(!eaf_sendspin_player_write(&player, past, pcm, sizeof(pcm)));
    CHECK(player.frames_written == 4 && player.frames_dropped == 2 && recorded_frames == 4);

    /* Backpressured sink drops the unaccepted remainder. */
    sink_limit = 1;
    CHECK(!eaf_sendspin_player_write(&player, future, pcm, sizeof(pcm)));
    CHECK(player.frames_written == 5 && player.frames_dropped == 3 && recorded_frames == 5);
    sink_limit = UINT32_MAX;
    eaf_sendspin_player_finish(&player);
    CHECK(!player.active);

    /* Mono is expanded to stereo. */
    recorded_frames = 0;
    eaf_sendspin_player_init(&player, record, NULL);
    eaf_sendspin_stream_start_t mono = make_start(1);
    CHECK(!eaf_sendspin_player_begin(&player, &filter, &mono));
    uint8_t one[2];
    put_le16(one, 3000);
    CHECK(!eaf_sendspin_player_write(&player, future, one, sizeof(one)));
    CHECK(player.frames_written == 1 && recorded_frames == 1);
    CHECK(recorded[0] == 3000 * 65536 && recorded[1] == 3000 * 65536);
    eaf_sendspin_player_finish(&player);

    /* 24-bit PCM is sign-extended and scaled to Q1.31. */
    recorded_frames = 0;
    eaf_sendspin_player_init(&player, record, NULL);
    eaf_sendspin_stream_start_t s24 = make_start(2);
    s24.bit_depth = 24;
    CHECK(!eaf_sendspin_player_begin(&player, &filter, &s24));
    uint8_t pcm24[6];
    put_le24(pcm24 + 0, 1000);
    put_le24(pcm24 + 3, -1000);
    CHECK(!eaf_sendspin_player_write(&player, future, pcm24, sizeof(pcm24)));
    CHECK(player.frames_written == 1 && recorded_frames == 1);
    CHECK(recorded[0] == 1000 * 256 && recorded[1] == -1000 * 256);
    eaf_sendspin_player_finish(&player);

    /* 32-bit PCM is already Q1.31. */
    recorded_frames = 0;
    eaf_sendspin_player_init(&player, record, NULL);
    eaf_sendspin_stream_start_t s32 = make_start(2);
    s32.bit_depth = 32;
    CHECK(!eaf_sendspin_player_begin(&player, &filter, &s32));
    uint8_t pcm32[8];
    put_le32(pcm32 + 0, 0x40000000);
    put_le32(pcm32 + 4, (int32_t)0xC0000000u);
    CHECK(!eaf_sendspin_player_write(&player, future, pcm32, sizeof(pcm32)));
    CHECK(player.frames_written == 1 && recorded_frames == 1);
    CHECK(recorded[0] == 0x40000000 && recorded[1] == (int32_t)0xC0000000u);
    eaf_sendspin_player_finish(&player);

    /* Rate control resamples into the sink to hold the target latency. With a
       persistent 200 ms measurement against a 100 ms target the correction is
       negative, so fewer output frames are written than source frames. */
    counted = 0;
    eaf_sendspin_player_init(&player, count_sink, NULL);
    eaf_sendspin_player_set_rate_control(&player, 100.0);
    CHECK(!eaf_sendspin_player_begin(&player, &filter, &start));
    CHECK(player.rate_control);
    uint8_t block[128u * 4u];
    memset(block, 0, sizeof(block));
    uint32_t source_frames = 0;
    for (unsigned i = 0; i < 20; ++i) {
        now += 200000u;
        int64_t late = eaf_sendspin_compute_server_time(&filter, (int64_t)now + 200000);
        CHECK(!eaf_sendspin_player_write(&player, late, block, sizeof(block)));
        source_frames += 128u;
    }
    CHECK(player.rate_ppm < 0);
    CHECK(counted == player.frames_written && player.frames_written < source_frames);
    eaf_sendspin_player_finish(&player);

    compressed_stream_test(argv[1]);
    incremental_stream_test(argv[2]);

    /* Unsupported format and invalid arguments. */
    eaf_sendspin_stream_start_t bad = make_start(2);
    bad.bit_depth = 20;
    CHECK(eaf_sendspin_player_begin(&player, &filter, &bad) == EAF_UNSUPPORTED);
    eaf_sendspin_player_t no_sink;
    eaf_sendspin_player_init(&no_sink, NULL, NULL);
    CHECK(eaf_sendspin_player_begin(&no_sink, &filter, &start) == EAF_INVALID);
    puts("sendspin player PASS");
    return 0;
}
