#include "speaker.h"
#include <eaf/eaf_bt_esp_idf.h>
#include <esp_a2dp_api.h>
#include <esp_avrc_api.h>
#include <esp_gap_bt_api.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs.h>
#include <sdkconfig.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* All radio policy and AVRCP transactions run in one owner task. Callbacks
 * copy bounded events; no borrowed metadata pointers escape the callback. */
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
} event_kind_t;
typedef struct {
    event_kind_t kind;
    unsigned value;
    uint32_t extra;
    uint8_t peer[6];
    char text[128];
} event_t;
static QueueHandle_t events;
static atomic_uint audio_level = ATOMIC_VAR_INIT(127u);
static atomic_bool overflow;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static speaker_status_t published;
static bool allow_pairing;
static uint8_t bonds[16][6];
static int bond_count;
static const char *TAG = "eaf_speaker";

/* Owner-task state. */
static speaker_status_t status = {.volume = 127, .playback_status = 0xff};
static bool ct_connected, tg_connected, volume_subscribed;
static esp_avrc_rn_evt_cap_mask_t capabilities;
static uint8_t last_peer[6];
static bool have_peer, automatic = true, attempting, cancelling, forgetting;
static unsigned attempts;
static int64_t pair_until, retry_at, attempt_until, save_at;
static nvs_handle_t settings;
static bool settings_open, settings_dirty;

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static void check(const char *operation, esp_err_t err) {
    if (err != ESP_OK)
        ESP_LOGW(TAG, "%s: %s", operation, esp_err_to_name(err));
}

static void post(const event_t *event) {
    if (xQueueSend(events, event, 0) != pdTRUE)
        atomic_store(&overflow, true);
}

int speaker_command(speaker_command_t command, unsigned value) {
    if (!events || command > SPEAKER_FORGET || command < SPEAKER_STATUS ||
        (command == SPEAKER_VOLUME && value > 127) || (command == SPEAKER_MUTE && value > 1))
        return EAF_INVALID;
    event_t event = {.kind = COMMAND, .value = (unsigned)command, .extra = value};
    return xQueueSend(events, &event, 0) == pdTRUE ? EAF_OK : EAF_TIMEOUT;
}

unsigned speaker_audio_level(void) {
    return atomic_load(&audio_level);
}

void speaker_get_status(speaker_status_t *out) {
    portENTER_CRITICAL(&status_lock);
    *out = published;
    portEXIT_CRITICAL(&status_lock);
}

static void publish(void) {
    atomic_store(&audio_level, (unsigned)status.volume | (status.muted ? 128u : 0u));
    portENTER_CRITICAL(&status_lock);
    published = status;
    allow_pairing = status.pairing && !forgetting;
    portEXIT_CRITICAL(&status_lock);
}

static bool pairing_allowed(const uint8_t *peer) {
    portENTER_CRITICAL(&status_lock);
    bool allowed = allow_pairing;
    for (int i = 0; i < bond_count; ++i)
        allowed |= memcmp(bonds[i], peer, 6) == 0;
    portEXIT_CRITICAL(&status_lock);
    return allowed;
}

static void refresh_bonds(void) {
    esp_bd_addr_t list[16];
    int count = 16;
    esp_err_t err = esp_bt_gap_get_bond_device_list(&count, list);
    if (err != ESP_OK) {
        check("read bonds", err);
        return;
    }
    portENTER_CRITICAL(&status_lock);
    bond_count = count;
    memcpy(bonds, list, (size_t)count * sizeof(list[0]));
    portEXIT_CRITICAL(&status_lock);
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
    case ESP_BT_GAP_CFM_REQ_EVT:
        check("confirm pairing", esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda,
                                                              pairing_allowed(param->cfm_req.bda)));
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
        bool accept = !param->pin_req.min_16_digit && pairing_allowed(param->pin_req.bda);
        check("legacy PIN", esp_bt_gap_pin_reply(param->pin_req.bda, accept, 4, pin));
        break;
    }
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        event_t msg = {.kind = AUTH, .value = param->auth_cmpl.stat};
        memcpy(msg.peer, param->auth_cmpl.bda, 6);
        post(&msg);
        break;
    }
    case ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT:
        post(&(event_t){.kind = BOND_REMOVED, .value = param->remove_bond_dev_cmpl.status});
        break;
    default:
        break;
    }
}

static void ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    event_t msg = {0};
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        msg.kind = CT_LINK;
        msg.value = param->conn_stat.connected;
        memcpy(msg.peer, param->conn_stat.remote_bda, 6);
        break;
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        msg.kind = CAPS;
        msg.value = param->get_rn_caps_rsp.evt_set.bits;
        break;
    case ESP_AVRC_CT_METADATA_RSP_EVT: {
        msg.kind = METADATA;
        msg.value = param->meta_rsp.attr_id;
        size_t length = param->meta_rsp.attr_length > 0 ? (size_t)param->meta_rsp.attr_length : 0;
        if (length >= sizeof(msg.text))
            length = sizeof(msg.text) - 1;
        if (param->meta_rsp.attr_text)
            memcpy(msg.text, param->meta_rsp.attr_text, length);
        /* Do not allow metadata to inject terminal control sequences. */
        for (size_t i = 0; i < length; ++i)
            if ((unsigned char)msg.text[i] < 32u || msg.text[i] == 127)
                msg.text[i] = ' ';
        break;
    }
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        if (param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE)
            msg.kind = TRACK;
        else if (param->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
            msg.kind = PLAY_STATUS;
            msg.value = param->change_ntf.event_parameter.playback;
        } else if (param->change_ntf.event_id == ESP_AVRC_RN_PLAY_POS_CHANGED) {
            msg.kind = POSITION;
            msg.value = param->change_ntf.event_parameter.play_pos;
        } else
            return;
        msg.extra = 1; /* Re-arm a one-shot notification. */
        break;
    case ESP_AVRC_CT_PLAY_STATUS_RSP_EVT:
        msg.kind = PLAY_STATUS;
        msg.value = param->play_status_rsp.play_status;
        post(&msg);
        msg.kind = POSITION;
        msg.value = param->play_status_rsp.song_position;
        msg.extra = param->play_status_rsp.song_length;
        break;
    default:
        return;
    }
    post(&msg);
}

static void tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        post(&(event_t){.kind = TG_LINK, .value = param->conn_stat.connected});
        break;
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
        post(&(event_t){.kind = VOLUME, .value = param->set_abs_vol.volume & 127u});
        break;
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
        if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE)
            post(&(event_t){.kind = VOLUME_SUBSCRIBE});
        break;
    default:
        break;
    }
}

static void clear_track(void) {
    status.title[0] = status.artist[0] = status.album[0] = 0;
    status.position_ms = status.duration_ms = 0;
    status.playback_status = 0xff;
}

static void request_track(void) {
    if (!ct_connected)
        return;
    check("metadata",
          esp_avrc_ct_send_metadata_cmd(1, ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST |
                                               ESP_AVRC_MD_ATTR_ALBUM));
    check("play status", esp_avrc_ct_send_get_play_status_cmd(5));
}

static void subscribe(esp_avrc_rn_event_ids_t event, uint8_t label) {
    if (ct_connected &&
        esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &capabilities, event))
        check("subscribe", esp_avrc_ct_send_register_notification_cmd(
                               label, event, event == ESP_AVRC_RN_PLAY_POS_CHANGED ? 1 : 0));
}

static void volume_reply(esp_avrc_rn_rsp_t response) {
    esp_avrc_rn_param_t param = {.volume = status.muted ? 0 : status.volume};
    check("volume notification",
          esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, response, &param));
}

static void set_volume(unsigned value, bool muted, bool remote) {
    uint8_t before = status.muted ? 0 : status.volume;
    status.volume = (uint8_t)value;
    status.muted = muted;
    publish();
    /* A remote SetAbsoluteVolume already has a stack-generated response;
     * CHANGED is only for local changes, preventing volume echo loops. */
    if (!remote && tg_connected && volume_subscribed && before != (muted ? 0 : value)) {
        volume_reply(ESP_AVRC_RN_RSP_CHANGED);
        volume_subscribed = false;
    }
    settings_dirty = true;
    save_at = now_ms() + 2000;
    ESP_LOGI(TAG, "volume=%u mute=%d", status.volume, status.muted);
}

static void scan_mode(void) {
    bool available = !status.connected && !forgetting;
    check("scan mode",
          esp_bt_gap_set_scan_mode(available ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
                                   available && status.pairing ? ESP_BT_GENERAL_DISCOVERABLE
                                                               : ESP_BT_NON_DISCOVERABLE));
    publish();
}

static void disconnect(void) {
    eaf_bt_esp_idf_status_t link;
    eaf_bt_esp_idf_status(&link);
    if (link.connected || link.connecting || attempting) {
        uint8_t peer[6];
        memcpy(peer, attempting ? last_peer : link.peer, 6);
        check("disconnect", esp_a2d_sink_disconnect(peer));
    }
}

static void start_pairing(void) {
    automatic = false;
    disconnect();
    status.pairing = true;
    pair_until = now_ms() + 120000;
    scan_mode();
    ESP_LOGI(TAG, "pairing window: 120 seconds");
}

static void show_status(void) {
    ESP_LOGI(TAG, "connected=%d stream=%d pairing=%d volume=%u mute=%d play=%u pos=%lu/%lu ms",
             status.connected, status.streaming, status.pairing, status.volume, status.muted,
             status.playback_status, (unsigned long)status.position_ms,
             (unsigned long)status.duration_ms);
    ESP_LOGI(TAG, "title: %s | artist: %s | album: %s", status.title, status.artist, status.album);
}

static void handle_command(const event_t *event) {
    switch ((speaker_command_t)event->value) {
    case SPEAKER_STATUS:
        show_status();
        break;
    case SPEAKER_VOLUME:
        set_volume(event->extra, false, false);
        break;
    case SPEAKER_MUTE:
        set_volume(status.volume, event->extra != 0, false);
        break;
    case SPEAKER_PAIR:
        if (!forgetting)
            start_pairing();
        break;
    case SPEAKER_RECONNECT:
        if (!forgetting && !status.connected && !attempting) {
            status.pairing = false;
            automatic = true;
            attempts = 0;
            retry_at = now_ms();
            scan_mode();
        }
        break;
    case SPEAKER_DISCONNECT:
        automatic = false;
        status.pairing = false;
        disconnect();
        scan_mode();
        break;
    case SPEAKER_FORGET:
        automatic = false;
        forgetting = true;
        have_peer = false;
        status.pairing = false;
        disconnect();
        scan_mode();
        if (settings_open) {
            esp_err_t err = nvs_erase_key(settings, "peer");
            if (err != ESP_ERR_NVS_NOT_FOUND)
                check("forget peer", err);
            check("save forget", nvs_commit(settings));
        }
        break;
    }
}

static void handle_event(const event_t *event) {
    switch (event->kind) {
    case COMMAND:
        handle_command(event);
        break;
    case CT_LINK:
        ct_connected = event->value != 0;
        capabilities.bits = 0;
        clear_track();
        if (ct_connected) {
            check("notification capabilities", esp_avrc_ct_send_get_rn_capabilities_cmd(0));
            request_track();
        }
        break;
    case TG_LINK:
        tg_connected = event->value != 0;
        volume_subscribed = false;
        break;
    case VOLUME:
        /* Apply the peer's absolute volume even if the target control channel
         * is not up yet; it is only our local gain. */
        set_volume(event->value, false, true);
        break;
    case VOLUME_SUBSCRIBE:
        if (tg_connected) {
            volume_subscribed = true;
            volume_reply(ESP_AVRC_RN_RSP_INTERIM);
        }
        break;
    case CAPS:
        capabilities.bits = (uint16_t)event->value;
        subscribe(ESP_AVRC_RN_TRACK_CHANGE, 2);
        subscribe(ESP_AVRC_RN_PLAY_STATUS_CHANGE, 3);
        subscribe(ESP_AVRC_RN_PLAY_POS_CHANGED, 4);
        break;
    case TRACK:
        clear_track();
        request_track();
        subscribe(ESP_AVRC_RN_TRACK_CHANGE, 2);
        break;
    case PLAY_STATUS:
        if (ct_connected)
            status.playback_status = (uint8_t)event->value;
        if (event->extra)
            subscribe(ESP_AVRC_RN_PLAY_STATUS_CHANGE, 3);
        break;
    case POSITION:
        if (ct_connected) {
            status.position_ms = event->value;
            if (event->extra != 1)
                status.duration_ms = event->extra;
        }
        if (event->extra == 1)
            subscribe(ESP_AVRC_RN_PLAY_POS_CHANGED, 4);
        break;
    case METADATA:
        if (!ct_connected)
            break;
        if (event->value == ESP_AVRC_MD_ATTR_TITLE)
            memcpy(status.title, event->text, sizeof(status.title));
        else if (event->value == ESP_AVRC_MD_ATTR_ARTIST)
            memcpy(status.artist, event->text, sizeof(status.artist));
        else if (event->value == ESP_AVRC_MD_ATTR_ALBUM)
            memcpy(status.album, event->text, sizeof(status.album));
        ESP_LOGI(TAG, "metadata %u: %s", event->value, event->text);
        break;
    case AUTH:
        ESP_LOGI(TAG, "authentication status=0x%x", event->value);
        refresh_bonds();
        break;
    case BOND_REMOVED:
        ESP_LOGI(TAG, "bond removal status=0x%x", event->value);
        refresh_bonds();
        break;
    }
    publish();
}

static void save_settings(void) {
    if (!settings_open)
        return;
    if (have_peer && !forgetting)
        check("save peer", nvs_set_blob(settings, "peer", last_peer, sizeof(last_peer)));
    check("commit settings", nvs_commit(settings));
    settings_dirty = false;
}

static void tick(void) {
    int64_t now = now_ms();
    eaf_bt_esp_idf_status_t link;
    eaf_bt_esp_idf_status(&link);
    if (link.connected != status.connected) {
        status.connected = link.connected;
        attempting = cancelling = false;
        if (link.connected) {
            memcpy(last_peer, link.peer, sizeof(last_peer));
            have_peer = true;
            attempts = 0;
            status.pairing = false;
            automatic = !forgetting;
            settings_dirty = true;
            save_at = now + 2000;
            refresh_bonds();
        } else {
            clear_track();
            retry_at = now + 2000;
        }
        ESP_LOGI(TAG, "A2DP connected=%d", status.connected);
        scan_mode();
    }
    status.streaming = link.streaming;
    if (forgetting && !link.connected && !link.connecting && !attempting) {
        /* Remove one at a time; wait for the asynchronous list to change before
         * requesting the next. Polling also recovers a lost completion event. */
        static int64_t remove_at;
        if (esp_bt_gap_get_bond_device_num() == 0) {
            refresh_bonds();
            forgetting = false;
            start_pairing();
        } else if (now >= remove_at) {
            esp_bd_addr_t peer[1];
            int count = 1;
            if (esp_bt_gap_get_bond_device_list(&count, peer) == ESP_OK && count)
                check("remove bond", esp_bt_gap_remove_bond_device(peer[0]));
            remove_at = now + 2000;
        }
    }
    if (status.pairing && now >= pair_until) {
        status.pairing = false;
        automatic = have_peer;
        attempts = 0;
        retry_at = now;
        scan_mode();
    }
    if (attempting && !link.connected && now >= attempt_until) {
        if (!cancelling) {
            disconnect();
            cancelling = true;
            attempt_until = now + 10000;
        } else {
            attempting = cancelling = false;
            /* Never start overlapping attempts if cancellation has not settled. */
            if (link.connecting)
                automatic = false;
            retry_at = now + (int64_t)(2000u << (attempts > 4 ? 4 : attempts));
        }
    }
    if (link.ready && automatic && have_peer && !forgetting && !status.pairing && !link.connected &&
        !link.connecting && !attempting && now >= retry_at) {
        if (attempts >= 5) {
            start_pairing();
        } else {
            esp_err_t err = esp_a2d_sink_connect(last_peer);
            ++attempts;
            check("reconnect", err);
            attempting = err == ESP_OK;
            attempt_until = now + 12000;
            retry_at = now + (int64_t)(2000u << attempts);
            ESP_LOGI(TAG, "reconnect attempt %u/5", attempts);
        }
    }
    if (settings_dirty && now >= save_at)
        save_settings();
    if (atomic_exchange(&overflow, false)) {
        ESP_LOGE(TAG, "control queue overflow; disconnecting to resynchronize");
        disconnect();
        clear_track();
        volume_subscribed = false;
    }
    publish();
}

static void control_task(void *arg) {
    (void)arg;
    refresh_bonds();
    if (!have_peer)
        start_pairing();
    else
        scan_mode();
    for (;;) {
        event_t event;
        if (xQueueReceive(events, &event, pdMS_TO_TICKS(100)) == pdTRUE)
            handle_event(&event);
        tick();
    }
}

int speaker_start(void) {
    events = xQueueCreate(32, sizeof(event_t));
    if (!events)
        return EAF_IO;
    /* Only the peer is persisted. Volume/mute start at full and unmuted so a
     * stale stored value can never boot the speaker silent. */
    status.volume = 127;
    status.muted = false;
    if (nvs_open("eaf_bt", NVS_READWRITE, &settings) == ESP_OK) {
        settings_open = true;
        size_t length = sizeof(last_peer);
        have_peer = nvs_get_blob(settings, "peer", last_peer, &length) == ESP_OK && length == 6;
        ESP_LOGI(TAG, "restored peer=%d", have_peer);
    }
    publish();
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_cb));
    esp_bt_io_cap_t capability = ESP_BT_IO_CAP_NONE;
    ESP_ERROR_CHECK(
        esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &capability, sizeof(capability)));
    esp_bt_pin_code_t pin = {0};
    ESP_ERROR_CHECK(esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin));
    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(CONFIG_EAF_BT_DEVICE_NAME));
    ESP_ERROR_CHECK(esp_avrc_ct_register_callback(ct_cb));
    ESP_ERROR_CHECK(esp_avrc_ct_init());
    ESP_ERROR_CHECK(esp_avrc_tg_register_callback(tg_cb));
    ESP_ERROR_CHECK(esp_avrc_tg_init());
    esp_avrc_rn_evt_cap_mask_t mask = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &mask, ESP_AVRC_RN_VOLUME_CHANGE);
    ESP_ERROR_CHECK(esp_avrc_tg_set_rn_evt_cap(&mask));
    if (xTaskCreate(control_task, "bt_control", 4096, NULL, 5, NULL) != pdPASS)
        return EAF_IO;
    return EAF_OK;
}
