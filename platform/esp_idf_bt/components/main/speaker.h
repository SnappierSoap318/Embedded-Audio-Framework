#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SPEAKER_STATUS,
    SPEAKER_VOLUME,
    SPEAKER_MUTE,
    SPEAKER_PAIR,
    SPEAKER_RECONNECT,
    SPEAKER_DISCONNECT,
    SPEAKER_FORGET
} speaker_command_t;

typedef struct {
    bool connected, streaming, pairing, muted;
    uint8_t volume, playback_status;
    char title[128], artist[128], album[128];
    uint32_t position_ms, duration_ms;
} speaker_status_t;

/* Call once with Bluedroid enabled, before A2DP registration. Volume starts at
 * full and unmuted every boot; only the last peer is persisted. */
int speaker_start(void);
/* Nonblocking command enqueue. EAF_OK means accepted, not completed.
 * Volume uses AVRCP units 0..127; mute uses 0/1. Pair lasts 120 seconds.
 * Forget disconnects, removes all bonds and the remembered peer, then pairs. */
int speaker_command(speaker_command_t command, unsigned value);
void speaker_get_status(speaker_status_t *status);
/* Atomic audio-owner snapshot: bits 0..6 volume, bit 7 mute. */
unsigned speaker_audio_level(void);
