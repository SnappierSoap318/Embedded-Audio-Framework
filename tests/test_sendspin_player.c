#include "check.h"
#include <eaf/eaf_hal.h>
#include <eaf/eaf_sendspin_player.h>
#include <string.h>

static eaf_sendspin_player_t player;
static int32_t storage[256];
static uint64_t now;

uint64_t __wrap_hal_monotonic_time_us(void) {
    return now;
}

static void put_le16(uint8_t *dst, int16_t value) {
    uint16_t raw = (uint16_t)value;
    dst[0] = (uint8_t)raw;
    dst[1] = (uint8_t)(raw >> 8);
}

static eaf_sendspin_stream_start_t make_start(uint8_t channels) {
    return (eaf_sendspin_stream_start_t){.codec = EAF_SENDPIN_CODEC_PCM,
                                         .sample_rate = 44100,
                                         .channels = channels,
                                         .bit_depth = 16};
}

static void pull_two(int32_t *out) {
    eaf_buffer_t buffer = {.samples = out, .frame_count = 2, .capacity_frames = 2};
    buffer.capacity_samples = 4;
    CHECK(!eaf_reservoir_pull(&player.reservoir, &buffer));
    CHECK(buffer.frame_count == 2);
}

int main(void) {
    eaf_sendspin_time_filter_t filter;
    eaf_sendspin_time_filter_init(&filter);
    eaf_sendspin_time_filter_update(&filter, 0, 100, 1000000u);
    eaf_sendspin_time_filter_update(&filter, 0, 100, 2000000u);
    CHECK(eaf_sendspin_time_synchronized(&filter));

    eaf_sendspin_player_init(&player, storage, 256u);
    now = 5000000u;
    eaf_sendspin_stream_start_t start = make_start(2);
    CHECK(!eaf_sendspin_player_begin(&player, &filter, &start));
    CHECK(eaf_sendspin_player_capacity_ms(&player) == 5u);
    CHECK(eaf_sendspin_player_buffer_bytes(&player) == 1024u);
    CHECK(eaf_sendspin_player_begin(&player, &filter, &start) == EAF_STATE); /* already active */

    /* Prefill past the 3/4 watermark so the reservoir leaves PREBUFFERING. */
    uint8_t pcm[192u * 4u];
    memset(pcm, 0, sizeof(pcm));
    put_le16(pcm + 0, 1000);
    put_le16(pcm + 2, -1000);
    put_le16(pcm + 4, 2000);
    put_le16(pcm + 6, -2000);
    int64_t future = eaf_sendspin_compute_server_time(&filter, (int64_t)now + 1000000);
    CHECK(!eaf_sendspin_player_write(&player, future, pcm, sizeof(pcm)));
    CHECK(player.frames_written == 192 && player.frames_dropped == 0 && player.chunks == 1);
    CHECK(eaf_reservoir_level(&player.reservoir) == 192);
    int32_t out[4];
    pull_two(out);
    CHECK(out[0] == 1000 * 65536 && out[1] == -1000 * 65536);
    CHECK(out[2] == 2000 * 65536 && out[3] == -2000 * 65536);
    CHECK(eaf_reservoir_level(&player.reservoir) == 190);

    /* Scheduled in the past: dropped without conversion. */
    int64_t past = eaf_sendspin_compute_server_time(&filter, (int64_t)now - 1000000);
    CHECK(!eaf_sendspin_player_write(&player, past, pcm, sizeof(pcm)));
    CHECK(player.frames_written == 192 && player.frames_dropped == 192 && player.chunks == 2);
    CHECK(eaf_reservoir_level(&player.reservoir) == 190);

    eaf_sendspin_player_finish(&player);
    CHECK(!player.active);

    /* Mono is expanded to stereo; smaller capacity lowers the watermark. */
    eaf_sendspin_player_init(&player, storage, 8u);
    eaf_sendspin_stream_start_t mono = make_start(1);
    CHECK(!eaf_sendspin_player_begin(&player, &filter, &mono));
    uint8_t one[6u * 2u];
    memset(one, 0, sizeof(one));
    put_le16(one, 3000);
    future = eaf_sendspin_compute_server_time(&filter, (int64_t)now + 1000000);
    CHECK(!eaf_sendspin_player_write(&player, future, one, sizeof(one)));
    CHECK(player.frames_written == 6);
    pull_two(out);
    CHECK(out[0] == 3000 * 65536 && out[1] == 3000 * 65536);
    eaf_sendspin_player_finish(&player);

    /* Unsupported format is rejected. */
    eaf_sendspin_stream_start_t bad = make_start(2);
    bad.bit_depth = 24;
    CHECK(eaf_sendspin_player_begin(&player, &filter, &bad) == EAF_UNSUPPORTED);
    puts("sendspin player PASS");
    return 0;
}
