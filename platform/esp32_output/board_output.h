#pragma once
#include <eaf/eaf_hal.h>

/* Protocol-agnostic output owner: stereo Q1.31 reservoir, DSP pipeline, sink
   worker and controls. Board applications supply the sink hooks and a log sink,
   then drive this interface from their protocol adapter. */

typedef void (*eaf_board_log_fn)(const char *message);
typedef eaf_sink_t *(*eaf_board_sink_source_fn)(void);
typedef int (*eaf_board_sink_init_fn)(void);
typedef int (*eaf_board_sink_pause_fn)(bool paused);
/* Frames still queued in the sink, subtracted from elapsed playback. */
typedef uint32_t (*eaf_board_delay_fn)(void);

typedef struct {
    int audio_cpu; /* -1 unrestricted; otherwise an OS CPU index. */
    eaf_board_log_fn log;
    int32_t *storage;
    uint32_t capacity_frames;
    /* Optional platform hooks; NULL falls back to the board_sink symbols below. */
    eaf_board_sink_source_fn sink;
    eaf_board_sink_init_fn sink_init;
    eaf_board_sink_pause_fn sink_pause;
    eaf_board_delay_fn delay;
} eaf_board_output_config_t;

typedef struct {
    uint32_t elapsed_ms, buffer_bytes, queued_bytes;
    /* True only after the output owner observes actual playback. */
    bool started;
} eaf_board_playback_t;

int eaf_board_output_init(const eaf_board_output_config_t *config);
/* Selected storage provider fills the reservoir backing store. */
int eaf_board_output_storage(int32_t **storage, uint32_t *frames);
/* Format is the source format (1..2 channels). ready_frames is the reservoir
   prefill required before release; held keeps the audio worker gated. */
int eaf_board_output_start(const eaf_format_t *format, uint32_t ready_frames, bool held);
/* Transport-owner only: cancel the previous stream (including pending EOF),
   join its worker and retire DMA before configuring a fresh reservoir. */
int eaf_board_output_replace(const eaf_format_t *format, uint32_t ready_frames, bool held);
/* Writes Q1.31 frames; returns the count accepted. */
uint32_t eaf_board_output_write(const int32_t *samples, uint32_t frames);
void eaf_board_output_finish(void);
void eaf_board_output_stop(void);
int eaf_board_output_pause(bool paused);
int eaf_board_output_release(void);
int eaf_board_output_volume(int32_t left, int32_t right);
bool eaf_board_output_failed(void);
bool eaf_board_output_snapshot(eaf_board_playback_t *playback);
uint32_t eaf_board_output_capacity_frames(void);
uint32_t eaf_board_output_level(void);
uint32_t eaf_board_output_underruns(void);
uint32_t eaf_board_output_queue_min(void);
uint32_t eaf_board_output_process_calls(void);
/* Transport-thread snapshot: active=1, released=2, pause request=4, ack=8, done=16. */
uint32_t eaf_board_output_flags(void);

/* Implemented by the application's selected sink backend, or injected above. */
#if defined(__GNUC__)
#define EAF_BOARD_WEAK __attribute__((weak))
#else
#define EAF_BOARD_WEAK
#endif
eaf_sink_t *board_sink(void) EAF_BOARD_WEAK;
int board_sink_init(void) EAF_BOARD_WEAK;
int board_sink_pause(bool paused) EAF_BOARD_WEAK;
#undef EAF_BOARD_WEAK
