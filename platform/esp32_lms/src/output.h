#pragma once
#include <eaf/eaf_hal.h>
#include <eaf/eaf_lms_client.h>
int board_output_init(void);
eaf_lms_callbacks_t board_output_callbacks(void);
bool board_output_failed(void);
uint32_t board_output_underruns(void);
uint32_t board_output_queue_min(void);
uint32_t board_output_process_calls(void);
/* Transport-thread snapshot: active=1, released=2, pause request=4, ack=8, done=16. */
uint32_t board_output_flags(void);
bool board_output_snapshot(eaf_lms_playback_t *snapshot);
/* Selected at build time; called by the sole output owner. */
eaf_sink_t *board_sink(void);
int board_sink_init(void);
int board_sink_pause(bool paused);

const char *board_wifi_ssid(void);
const char *board_wifi_password(void);
