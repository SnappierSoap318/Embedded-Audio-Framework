#include "output.h"
#include "diagnostics.h"

/* Adapts the LMS client callbacks to the protocol-agnostic output owner. */
static void lms_log(const char *message) {
    board_log("%s", message);
}

int board_output_init(void) {
    int32_t *storage_store;
    uint32_t storage_frames;
    int rc = eaf_board_output_storage(&storage_store, &storage_frames);
    if (rc)
        return rc;
    eaf_board_output_config_t config = {.audio_cpu = CONFIG_EAF_BOARD_AUDIO_CPU,
                                        .log = lms_log,
                                        .storage = storage_store,
                                        .capacity_frames = storage_frames};
    return eaf_board_output_init(&config);
}

static int lms_start(void *ctx, const eaf_format_t *format) {
    (void)ctx;
    return eaf_board_output_start(format, eaf_board_output_capacity_frames() * 3u / 4u, false);
}

static int lms_start_buffered(void *ctx, const eaf_format_t *format,
                              const eaf_lms_buffer_request_t *request,
                              eaf_lms_buffer_limits_t *limits) {
    (void)ctx;
    int rc = eaf_lms_buffer_limits(format, request, eaf_board_output_capacity_frames(),
                                   eaf_board_output_capacity_frames() * 3u / 4u, limits);
    if (rc)
        return rc;
    board_log("Buffer request=%u B/%u ms ready=%u/%u frames clamped=%u", request->stream_bytes,
              request->output_ms, limits->ready_frames, limits->capacity_frames,
              limits->clamped ? 1u : 0u);
    return eaf_board_output_start(format, limits->ready_frames, true);
}

static uint32_t lms_pcm(void *ctx, const int32_t *samples, uint32_t frames) {
    (void)ctx;
    return eaf_board_output_write(samples, frames);
}

static void lms_eof(void *ctx) {
    (void)ctx;
    eaf_board_output_finish();
}

static void lms_stop(void *ctx) {
    (void)ctx;
    eaf_board_output_stop();
}

static int lms_pause_output(void *ctx, bool paused) {
    (void)ctx;
    return eaf_board_output_pause(paused);
}

static int lms_set_volume(void *ctx, int32_t left, int32_t right) {
    (void)ctx;
    return eaf_board_output_volume(left, right);
}

static int lms_release_output(void *ctx) {
    (void)ctx;
    return eaf_board_output_release();
}

eaf_lms_callbacks_t board_output_callbacks(void) {
    return (eaf_lms_callbacks_t){.start = lms_start,
                                 .pcm = lms_pcm,
                                 .eof = lms_eof,
                                 .stop = lms_stop,
                                 .pause = lms_pause_output,
                                 .volume = lms_set_volume,
                                 .start_buffered = lms_start_buffered,
                                 .release = lms_release_output};
}

bool board_output_failed(void) {
    return eaf_board_output_failed();
}

bool board_output_snapshot(eaf_lms_playback_t *snapshot) {
    eaf_board_playback_t generic;
    if (!eaf_board_output_snapshot(&generic))
        return false;
    *snapshot = (eaf_lms_playback_t){generic.elapsed_ms, generic.buffer_bytes, generic.queued_bytes,
                                     generic.started};
    return true;
}

uint32_t board_output_underruns(void) {
    return eaf_board_output_underruns();
}
uint32_t board_output_queue_min(void) {
    return eaf_board_output_queue_min();
}
uint32_t board_output_process_calls(void) {
    return eaf_board_output_process_calls();
}
uint32_t board_output_flags(void) {
    return eaf_board_output_flags();
}
