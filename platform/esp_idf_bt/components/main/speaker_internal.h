#pragma once
#include "speaker.h"
#include <esp_avrc_api.h>
#include <freertos/FreeRTOS.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    COMMAND,
    CT_LINK,
    TG_LINK,
    VOLUME,
    VOLUME_SUBSCRIBE,
    CAPS,
    TRACK,
    PLAY_STATUS,
    POSITION,
    METADATA,
    AUTH,
    BOND_REMOVED
} speaker_event_kind_t;

#define SPEAKER_TEXT_MAX 128u
#define SPEAKER_TEXT_SLOTS 4u

typedef struct {
    uint8_t slot;
    uint8_t length;
} speaker_metadata_t;

typedef union {
    uint8_t peer[6];
    speaker_metadata_t metadata;
} speaker_event_payload_t;

typedef struct {
    speaker_event_kind_t kind;
    unsigned value;
    uint32_t extra;
    speaker_event_payload_t payload;
} speaker_event_t;

typedef struct {
    speaker_status_t status;
    bool ct_connected, tg_connected, volume_subscribed;
    esp_avrc_rn_evt_cap_mask_t capabilities;
    uint8_t last_peer[6];
    bool have_peer, forgetting;
} speaker_state_t;

extern speaker_state_t speaker;
extern portMUX_TYPE speaker_lock;
extern bool speaker_allow_pairing;
extern uint8_t speaker_bonds[16][6];
extern int speaker_bond_count;

int64_t speaker_now_ms(void);
void speaker_check(const char *operation, esp_err_t err);
bool speaker_post(const speaker_event_t *event);
char *speaker_text_acquire(uint8_t *slot, size_t *capacity);
const char *speaker_text_read(uint8_t slot);
void speaker_text_release(uint8_t slot);

void speaker_publish(void);
void speaker_disconnect(void);
void speaker_scan_mode(void);
void speaker_start_pairing(void);
void speaker_log_status(void);

void speaker_gap_register(void);
void speaker_gap_tick(int64_t now);
bool speaker_pairing_allowed(const uint8_t *peer);
void speaker_refresh_bonds(void);
void speaker_begin_forget(void);

void speaker_avrcp_register(void);
void speaker_avrcp_clear_track(void);
void speaker_avrcp_request_track(void);
void speaker_avrcp_subscribe(esp_avrc_rn_event_ids_t event, uint8_t label);
void speaker_avrcp_volume_reply(esp_avrc_rn_rsp_t response);

void speaker_nvs_init(void);
void speaker_nvs_touch(void);
void speaker_nvs_tick(int64_t now);
void speaker_nvs_forget_peer(void);
