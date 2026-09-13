#include "check.h"
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../platform/esp32_output/board_output.c"
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../platform/esp32_lms/src/output.c"
#include <eaf/eaf_sink_null.h>
static void wait_done(void) {
    uint64_t deadline = hal_monotonic_time_us() + 2000000u;
    while (!hal_atomic_get(&done)) {
        CHECK(hal_monotonic_time_us() < deadline);
        hal_sleep_ms(1);
    }
    CHECK(!board_output_failed());
}
int main(void) {
    CHECK(!board_output_init());
    eaf_lms_callbacks_t cb = board_output_callbacks();
    for (unsigned track = 0; track < 8; ++track) {
        eaf_format_t fmt = {track & 1u ? 44100 : 48000, 1, EAF_CH_FRONT_CENTER};
        CHECK(!cb.start(NULL, &fmt));
        CHECK(cb.start(NULL, &fmt) == EAF_STATE);
        CHECK(!cb.pause(NULL, true));
        CHECK(hal_atomic_get(&pause_ack));
        CHECK(!cb.volume(NULL, INT32_MAX, 0));
        int32_t mono[] = {1073741824, 1073741824};
        CHECK(cb.pcm(NULL, mono, 2) == 2);
        cb.eof(NULL);
        CHECK(!cb.pause(NULL, false));
        wait_done();
        eaf_lms_playback_t snapshot;
        CHECK(board_output_snapshot(&snapshot));
        CHECK(snapshot.started && snapshot.buffer_bytes == 32768 && !snapshot.queued_bytes);
        CHECK(reservoir.frames_read == 2 && !reservoir.underruns);
        eaf_null_sink_ctx_t *out = board_sink()->driver_data;
        int32_t *samples = out->samples[out->active ^ 1u];
        CHECK(samples[0] >= 134217700 && samples[0] <= 134217728);
        CHECK(samples[1] == 0 && samples[3] == 0);
        cb.stop(NULL);
        CHECK(!active && !audio.impl && pipeline.state == EAF_UNINITIALIZED);
    }
    eaf_format_t stereo = {48000, 2, 3};
    CHECK(!cb.start(NULL, &stereo));
    CHECK(!cb.pause(NULL, true));
    CHECK(!cb.volume(NULL, INT32_MAX, INT32_MAX));
    int32_t channels[] = {1073741824, -1073741824, 0, 1073741824};
    CHECK(cb.pcm(NULL, channels, 2) == 2);
    cb.eof(NULL);
    CHECK(!cb.pause(NULL, false));
    wait_done();
    eaf_null_sink_ctx_t *out = board_sink()->driver_data;
    int32_t *samples = out->samples[out->active ^ 1u];
    CHECK(samples[0] > 134217700 && samples[1] < -134217700);
    CHECK(samples[2] == 0 && samples[3] > 134217700);
    cb.stop(NULL);
    CHECK(!cb.start(NULL, &stereo));
    cb.stop(NULL); /* Cancellation while prebuffering. */
    CHECK(!active && !board_output_failed());
    eaf_lms_buffer_request_t request = {0, 0, 2};
    eaf_lms_buffer_limits_t limits;
    CHECK(!cb.start_buffered(NULL, &stereo, &request, &limits));
    CHECK(limits.capacity_frames == 4096 && limits.ready_frames == 3072 && !limits.clamped);
    CHECK(!hal_atomic_get(&run_gate));
    CHECK(board_output_flags() == 1u && board_output_process_calls() == 0u);
    CHECK(!cb.pause(NULL, true));
    CHECK(board_output_flags() == 13u);
    CHECK(!cb.pause(NULL, false));
    CHECK(!hal_atomic_get(&run_gate));
    CHECK(cb.pcm(NULL, channels, 2) == 2);
    cb.eof(NULL);
    hal_sleep_ms(10);
    CHECK(!hal_atomic_get(&done));
    CHECK(!cb.release(NULL));
    wait_done();
    CHECK(reservoir.frames_read == 2 && !reservoir.underruns);
    CHECK(board_output_flags() == 19u && board_output_process_calls() > 0u);
    cb.stop(NULL);
    request.stream_bytes = 255u * 1024u;
    request.output_ms = 25500;
    CHECK(!cb.start_buffered(NULL, &stereo, &request, &limits));
    CHECK(limits.ready_frames == 4096 && limits.clamped);
    cb.stop(NULL); /* Cancellation while the worker is held. */
    CHECK(!active && !audio.impl && !board_output_failed());
    return 0;
}
