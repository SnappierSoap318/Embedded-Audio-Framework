#include "speaker_internal.h"
#include <eaf/eaf_bt_esp_idf.h>
#include <esp_gap_bt_api.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdatomic.h>
#include <string.h>

/* All radio policy and AVRCP transactions run in one owner task. Callbacks
 * copy bounded events; no borrowed metadata pointers escape the callback. */
static const char *TAG = "eaf_speaker";
static QueueHandle_t events;
static atomic_uint audio_level = ATOMIC_VAR_INIT(127u);
static atomic_bool overflow;
static speaker_status_t published;

speaker_state_t speaker;
portMUX_TYPE speaker_lock = portMUX_INITIALIZER_UNLOCKED;
bool speaker_allow_pairing;
uint8_t speaker_bonds[16][6];
int speaker_bond_count;

/* Metadata text lives outside the queue so only METADATA events pay for it. */
static char metadata_text[SPEAKER_TEXT_SLOTS][SPEAKER_TEXT_MAX];
static uint8_t metadata_used;

int64_t speaker_now_ms(void) {
    return esp_timer_get_time() / 1000;
}

void speaker_check(const char *operation, esp_err_t err) {
    if (err != ESP_OK)
        ESP_LOGW(TAG, "%s: %s", operation, esp_err_to_name(err));
}

char *speaker_text_acquire(uint8_t *slot, size_t *capacity) {
    portENTER_CRITICAL(&speaker_lock);
    for (uint8_t i = 0; i < SPEAKER_TEXT_SLOTS; ++i) {
        if (metadata_used & (uint8_t)(1u << i))
            continue;
        metadata_used |= (uint8_t)(1u << i);
        memset(metadata_text[i], 0, SPEAKER_TEXT_MAX);
        portEXIT_CRITICAL(&speaker_lock);
        *slot = i;
        *capacity = SPEAKER_TEXT_MAX;
        return metadata_text[i];
    }
    portEXIT_CRITICAL(&speaker_lock);
    return NULL;
}

const char *speaker_text_read(uint8_t slot) {
    return metadata_text[slot];
}

void speaker_text_release(uint8_t slot) {
    portENTER_CRITICAL(&speaker_lock);
    metadata_used &= (uint8_t)~(uint8_t)(1u << slot);
    portEXIT_CRITICAL(&speaker_lock);
}

bool speaker_post(const speaker_event_t *event) {
    if (xQueueSend(events, event, 0) != pdTRUE) {
        atomic_store(&overflow, true);
        return false;
    }
    return true;
}

int speaker_command(speaker_command_t command, unsigned value) {
    if (!events || command > SPEAKER_FORGET || command < SPEAKER_STATUS ||
        (command == SPEAKER_VOLUME && value > 127) || (command == SPEAKER_MUTE && value > 1))
        return EAF_INVALID;
    speaker_event_t event = {.kind = COMMAND, .value = (unsigned)command, .extra = value};
    return xQueueSend(events, &event, 0) == pdTRUE ? EAF_OK : EAF_TIMEOUT;
}

unsigned speaker_audio_level(void) {
    return atomic_load(&audio_level);
}

void speaker_get_status(speaker_status_t *out) {
    portENTER_CRITICAL(&speaker_lock);
    *out = published;
    portEXIT_CRITICAL(&speaker_lock);
}

void speaker_publish(void) {
    atomic_store(&audio_level,
                 (unsigned)speaker.status.volume | (speaker.status.muted ? 128u : 0u));
    portENTER_CRITICAL(&speaker_lock);
    published = speaker.status;
    speaker_allow_pairing = speaker.status.pairing && !speaker.forgetting;
    portEXIT_CRITICAL(&speaker_lock);
}

void speaker_scan_mode(void) {
    /* Headless device with no pairing button: whenever nothing is connected it is
     * both connectable and generally discoverable, so a source can (re)pair at
     * any time. While connected it is neither. */
    bool idle = !speaker.status.connected && !speaker.forgetting;
    speaker_check("scan mode", esp_bt_gap_set_scan_mode(
                                   idle ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
                                   idle ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE));
    speaker.status.pairing = idle;
    speaker_publish();
}

void speaker_disconnect(void) {
    eaf_bt_esp_idf_status_t link;
    eaf_bt_esp_idf_status(&link);
    if (link.connected || link.connecting)
        speaker_check("disconnect", eaf_bt_esp_idf_disconnect());
}

void speaker_start_pairing(void) {
    speaker_disconnect();
    speaker_scan_mode();
}

void speaker_log_status(void) {
    ESP_LOGI(TAG, "connected=%d stream=%d pairing=%d volume=%u mute=%d play=%u pos=%lu/%lu ms",
             speaker.status.connected, speaker.status.streaming, speaker.status.pairing,
             speaker.status.volume, speaker.status.muted, speaker.status.playback_status,
             (unsigned long)speaker.status.position_ms, (unsigned long)speaker.status.duration_ms);
    ESP_LOGI(TAG, "title: %s | artist: %s | album: %s", speaker.status.title, speaker.status.artist,
             speaker.status.album);
}

static void handle_command(const speaker_event_t *event);
static void set_volume(unsigned value, bool muted, bool remote);

static void handle_command(const speaker_event_t *event) {
    switch ((speaker_command_t)event->value) {
    case SPEAKER_STATUS:
        speaker_log_status();
        break;
    case SPEAKER_VOLUME:
        set_volume(event->extra, false, false);
        break;
    case SPEAKER_MUTE:
        set_volume(speaker.status.volume, event->extra != 0, false);
        break;
    case SPEAKER_PAIR:
        if (!speaker.forgetting)
            speaker_start_pairing();
        break;
    case SPEAKER_RECONNECT:
        if (!speaker.forgetting)
            speaker_start_pairing();
        break;
    case SPEAKER_DISCONNECT:
        speaker_disconnect();
        speaker_scan_mode();
        break;
    case SPEAKER_FORGET:
        speaker_begin_forget();
        break;
    }
}

static void set_volume(unsigned value, bool muted, bool remote) {
    uint8_t before = speaker.status.muted ? 0 : speaker.status.volume;
    speaker.status.volume = (uint8_t)value;
    speaker.status.muted = muted;
    speaker_publish();
    /* A remote SetAbsoluteVolume already has a stack-generated response;
     * CHANGED is only for local changes, preventing volume echo loops. */
    if (!remote && speaker.tg_connected && speaker.volume_subscribed &&
        before != (muted ? 0 : value)) {
        speaker_avrcp_volume_reply(ESP_AVRC_RN_RSP_CHANGED);
        speaker.volume_subscribed = false;
    }
    speaker_nvs_touch();
    ESP_LOGI(TAG, "volume=%u mute=%d", speaker.status.volume, speaker.status.muted);
}

static void handle_event(const speaker_event_t *event) {
    switch (event->kind) {
    case COMMAND:
        handle_command(event);
        break;
    case CT_LINK:
        speaker.ct_connected = event->value != 0;
        speaker.capabilities.bits = 0;
        speaker_avrcp_clear_track();
        if (speaker.ct_connected) {
            speaker_check("notification capabilities", esp_avrc_ct_send_get_rn_capabilities_cmd(0));
            speaker_avrcp_request_track();
        }
        break;
    case TG_LINK:
        speaker.tg_connected = event->value != 0;
        speaker.volume_subscribed = false;
        break;
    case VOLUME:
        /* Apply the peer's absolute volume even if the target control channel
         * is not up yet; it is only our local gain. */
        set_volume(event->value, false, true);
        break;
    case VOLUME_SUBSCRIBE:
        if (speaker.tg_connected) {
            speaker.volume_subscribed = true;
            speaker_avrcp_volume_reply(ESP_AVRC_RN_RSP_INTERIM);
        }
        break;
    case CAPS:
        speaker.capabilities.bits = (uint16_t)event->value;
        speaker_avrcp_subscribe(ESP_AVRC_RN_TRACK_CHANGE, 2);
        speaker_avrcp_subscribe(ESP_AVRC_RN_PLAY_STATUS_CHANGE, 3);
        speaker_avrcp_subscribe(ESP_AVRC_RN_PLAY_POS_CHANGED, 4);
        break;
    case TRACK:
        speaker_avrcp_clear_track();
        speaker_avrcp_request_track();
        speaker_avrcp_subscribe(ESP_AVRC_RN_TRACK_CHANGE, 2);
        break;
    case PLAY_STATUS:
        if (speaker.ct_connected)
            speaker.status.playback_status = (uint8_t)event->value;
        if (event->extra)
            speaker_avrcp_subscribe(ESP_AVRC_RN_PLAY_STATUS_CHANGE, 3);
        break;
    case POSITION:
        if (speaker.ct_connected) {
            speaker.status.position_ms = event->value;
            if (event->extra != 1)
                speaker.status.duration_ms = event->extra;
        }
        if (event->extra == 1)
            speaker_avrcp_subscribe(ESP_AVRC_RN_PLAY_POS_CHANGED, 4);
        break;
    case METADATA: {
        const char *text = speaker_text_read(event->payload.metadata.slot);
        if (speaker.ct_connected) {
            if (event->value == ESP_AVRC_MD_ATTR_TITLE)
                memcpy(speaker.status.title, text, sizeof(speaker.status.title));
            else if (event->value == ESP_AVRC_MD_ATTR_ARTIST)
                memcpy(speaker.status.artist, text, sizeof(speaker.status.artist));
            else if (event->value == ESP_AVRC_MD_ATTR_ALBUM)
                memcpy(speaker.status.album, text, sizeof(speaker.status.album));
            ESP_LOGI(TAG, "metadata %u: %s", event->value, text);
        }
        speaker_text_release(event->payload.metadata.slot);
        break;
    }
    case AUTH:
        ESP_LOGI(TAG, "authentication status=0x%x", event->value);
        /* The remembered peer no longer has our link key (it unpaired): drop the
         * stale bond and become discoverable instead of retrying forever. */
        if (event->value != ESP_BT_STATUS_SUCCESS && speaker.have_peer &&
            !speaker.status.connected && memcmp(event->payload.peer, speaker.last_peer, 6) == 0) {
            ESP_LOGW(TAG, "peer rejected the link key; returning to pairing");
            speaker_begin_forget();
        }
        speaker_refresh_bonds();
        break;
    case BOND_REMOVED:
        ESP_LOGI(TAG, "bond removal status=0x%x", event->value);
        speaker_refresh_bonds();
        break;
    }
    speaker_publish();
}

static void tick(void) {
    int64_t now = speaker_now_ms();
    eaf_bt_esp_idf_status_t link;
    eaf_bt_esp_idf_status(&link);
    if (link.connected != speaker.status.connected) {
        speaker.status.connected = link.connected;
        if (link.connected) {
            memcpy(speaker.last_peer, link.peer, sizeof(speaker.last_peer));
            speaker.have_peer = true;
            speaker.status.pairing = false;
            speaker_nvs_touch();
            speaker_refresh_bonds();
        } else {
            speaker_avrcp_clear_track();
        }
        ESP_LOGI(TAG, "A2DP connected=%d", speaker.status.connected);
        speaker_scan_mode();
    }
    speaker.status.streaming = link.streaming;
    speaker_gap_tick(now);
    speaker_nvs_tick(now);
    if (atomic_exchange(&overflow, false)) {
        ESP_LOGE(TAG, "control queue overflow; disconnecting to resynchronize");
        speaker_disconnect();
        speaker_avrcp_clear_track();
        speaker.volume_subscribed = false;
    }
    speaker_publish();
}

static void control_task(void *arg) {
    (void)arg;
    speaker_refresh_bonds();
    speaker_scan_mode();
    for (;;) {
        speaker_event_t event;
        if (xQueueReceive(events, &event, pdMS_TO_TICKS(100)) == pdTRUE)
            handle_event(&event);
        tick();
    }
}

int speaker_start(void) {
    events = xQueueCreate(32, sizeof(speaker_event_t));
    if (!events)
        return EAF_IO;
    /* Only the peer is persisted. Volume/mute start at full and unmuted so a
     * stale stored value can never boot the speaker silent. */
    speaker.status.volume = 127;
    speaker.status.muted = false;
    speaker_nvs_init();
    speaker_publish();
    speaker_gap_register();
    speaker_avrcp_register();
    if (xTaskCreate(control_task, "bt_control", 4096, NULL, 5, NULL) != pdPASS)
        return EAF_IO;
    return EAF_OK;
}
