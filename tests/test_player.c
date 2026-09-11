#include "check.h"
#include <eaf/eaf_player.h>
#include <eaf/eaf_sink_null.h>
typedef struct {
    uint64_t position;
    bool fail;
} source_ctx;
static int read_source(eaf_source_t *s, int32_t *samples, uint32_t capacity, uint32_t *frames) {
    source_ctx *ctx = s->ctx;
    *frames = 0;
    if (ctx->fail)
        return EAF_IO;
    uint64_t left = s->total_frames - ctx->position;
    uint32_t n = left < capacity ? (uint32_t)left : capacity;
    for (uint32_t i = 0; i < n; ++i) {
        samples[(size_t)i * 2u] = (int32_t)(ctx->position + i + 1u);
        samples[(size_t)i * 2u + 1u] = -samples[(size_t)i * 2u];
    }
    ctx->position += n;
    *frames = n;
    return EAF_OK;
}
static int seek_source(eaf_source_t *s, uint64_t frame) {
    if (frame > s->total_frames)
        return EAF_INVALID;
    source_ctx *ctx = s->ctx;
    ctx->position = frame;
    return EAF_OK;
}
static const struct eaf_source_ops source_ops = {read_source, seek_source};
static eaf_null_sink_ctx_t sink_ctx;
static eaf_player_t player;
static void await_finished(eaf_reservoir_t *r) {
    uint64_t deadline = hal_monotonic_time_us() + 2000000u;
    while (!hal_atomic_get(&r->finished)) {
        CHECK(hal_monotonic_time_us() < deadline);
        hal_sleep_ms(1);
    }
}
int main(void) {
    eaf_reservoir_t r;
    int32_t storage[128];
    eaf_format_t fmt = {48000, 2, 3};
    CHECK(eaf_reservoir_init(&r, storage, 64, fmt, 48) == 0);
    source_ctx ctx = {0};
    eaf_source_t source = {&source_ops, &ctx, fmt, 19};
    eaf_sink_t sink = {&eaf_null_sink_ops, &sink_ctx};
    eaf_volume_ctx_t volume = {{INT32_MAX, INT32_MAX, 0, 0}};
    eaf_node_t node = {"vol", EAF_NODE_STAGE_POST_PROCESS, &eaf_volume_ops, &volume};
    eaf_node_t *nodes[] = {&node};
    eaf_pipeline_config_t config = {&r, nodes, 1, &sink};
    eaf_pipeline_t p = {0};
    CHECK(eaf_pipeline_init(&p, &config) == 0 && eaf_pipeline_configure(&p, 16) == 0);
    r.high_watermark = 64;
    CHECK(eaf_player_init(&player, &p, &source, &volume) == EAF_INVALID);
    r.high_watermark = 48;
    r.backpressure_low = 0;
    CHECK(eaf_player_init(&player, &p, &source, &volume) == EAF_INVALID);
    r.backpressure_low = 32;
    CHECK(eaf_player_init(&player, &p, &source, &volume) == 0);
    CHECK(eaf_player_start(&player) == 0);
    await_finished(&r);
    CHECK(eaf_player_step(&player) == 0);
    CHECK(sink_ctx.samples[0][0] == 1 && sink_ctx.samples[0][30] == 16);
    CHECK(eaf_player_step(&player) == EAF_EOF);
    CHECK(sink_ctx.samples[1][0] == 17 && sink_ctx.samples[1][4] == 19);
    for (size_t i = 6; i < 32; ++i)
        CHECK(sink_ctx.samples[1][i] == 0);
    CHECK(r.frames_read == 19 && r.underruns == 0 && p.state == EAF_STOPPED &&
          !player.decoder.impl);
    /* Seek while running discards old queued data and starts a new stream. */
    CHECK(eaf_player_seek(&player, 0) == 0 && eaf_player_start(&player) == 0);
    await_finished(&r);
    CHECK(eaf_player_seek(&player, 5) == 0);
    await_finished(&r);
    CHECK(eaf_player_step(&player) == EAF_EOF);
    CHECK(r.frames_read == 14 && sink_ctx.samples[0][0] == 6 && sink_ctx.samples[0][26] == 19);
    CHECK(eaf_player_seek(&player, 0) == 0 && eaf_player_start(&player) == 0);
    await_finished(&r);
    eaf_command_t queued_seek = {EAF_COMMAND_SEEK, 5, 0};
    CHECK(eaf_player_command(&player, &queued_seek));
    int seek_result = eaf_player_step(&player);
    CHECK(seek_result == EAF_OK || seek_result == EAF_EOF);
    if (seek_result == EAF_OK) {
        await_finished(&r);
        CHECK(eaf_player_step(&player) == EAF_EOF);
    }
    CHECK(r.frames_read == 14 && !r.underruns);
    /* Gain crosses queue, updates only at the owner block boundary. */
    CHECK(eaf_player_seek(&player, 0) == 0 && eaf_player_start(&player) == 0);
    await_finished(&r);
    eaf_command_t command = {EAF_COMMAND_GAIN, 0, 0};
    CHECK(eaf_player_command(&player, &command));
    CHECK(volume.gain[0] == INT32_MAX);
    CHECK(eaf_player_step(&player) == 0 && volume.gain[0] == 0);
    for (size_t i = 0; i < 32; ++i)
        CHECK(sink_ctx.samples[0][i] == 0);
    CHECK(eaf_player_stop(&player) == 0);
    command.type = EAF_COMMAND_SEEK;
    command.frame = 20;
    CHECK(!eaf_player_command(&player, &command));
    /* Block the producer inside a partial write; STOP must wake and join it. */
    source.total_frames = 1000000;
    CHECK(eaf_player_seek(&player, 0) == 0);
    CHECK(eaf_player_start(&player) == 0);
    uint64_t deadline = hal_monotonic_time_us() + 2000000u;
    while (eaf_reservoir_level(&r) != 64u) {
        CHECK(hal_monotonic_time_us() < deadline);
        hal_sleep_ms(1);
    }
    command.type = EAF_COMMAND_STOP;
    CHECK(eaf_player_command(&player, &command));
    CHECK(eaf_player_step(&player) == EAF_EOF && !player.decoder.impl);
    /* Read failure ends playback with the original error. */
    ctx.fail = true;
    CHECK(eaf_player_start(&player) == 0);
    deadline = hal_monotonic_time_us() + 2000000u;
    while (!hal_atomic_get(&player.failed)) {
        CHECK(hal_monotonic_time_us() < deadline);
        hal_sleep_ms(1);
    }
    CHECK(eaf_player_step(&player) == EAF_IO && p.state == EAF_STOPPED);
    ctx.fail = false;
    source.total_frames = 0;
    CHECK(eaf_player_seek(&player, 0) == 0 && eaf_player_start(&player) == 0);
    await_finished(&r);
    CHECK(eaf_player_step(&player) == EAF_EOF && r.frames_read == 0 && !r.underruns);
    CHECK(eaf_player_deinit(&player) == 0 && !r.wake_producer);
    CHECK(eaf_pipeline_deinit(&p) == 0);
    return 0;
}
