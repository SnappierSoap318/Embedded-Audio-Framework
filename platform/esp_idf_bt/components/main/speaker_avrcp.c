#include "speaker_internal.h"
#include <esp_avrc_api.h>
#include <string.h>

void speaker_avrcp_clear_track(void) {
    speaker.status.title[0] = speaker.status.artist[0] = speaker.status.album[0] = 0;
    speaker.status.position_ms = speaker.status.duration_ms = 0;
    speaker.status.playback_status = 0xff;
}

void speaker_avrcp_request_track(void) {
    if (!speaker.ct_connected)
        return;
    speaker_check("metadata", esp_avrc_ct_send_metadata_cmd(1, ESP_AVRC_MD_ATTR_TITLE |
                                                                   ESP_AVRC_MD_ATTR_ARTIST |
                                                                   ESP_AVRC_MD_ATTR_ALBUM));
    speaker_check("play status", esp_avrc_ct_send_get_play_status_cmd(5));
}

void speaker_avrcp_subscribe(esp_avrc_rn_event_ids_t event, uint8_t label) {
    if (speaker.ct_connected &&
        esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &speaker.capabilities, event))
        speaker_check("subscribe",
                      esp_avrc_ct_send_register_notification_cmd(
                          label, event, event == ESP_AVRC_RN_PLAY_POS_CHANGED ? 1 : 0));
}

void speaker_avrcp_volume_reply(esp_avrc_rn_rsp_t response) {
    esp_avrc_rn_param_t param = {.volume = speaker.status.muted ? 0 : speaker.status.volume};
    speaker_check("volume notification",
                  esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, response, &param));
}

static void ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    speaker_event_t msg = {0};
    bool has_text = false;
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        msg.kind = CT_LINK;
        msg.value = param->conn_stat.connected;
        memcpy(msg.payload.peer, param->conn_stat.remote_bda, 6);
        break;
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        msg.kind = CAPS;
        msg.value = param->get_rn_caps_rsp.evt_set.bits;
        break;
    case ESP_AVRC_CT_METADATA_RSP_EVT: {
        uint8_t slot = 0;
        size_t capacity = 0;
        char *text = speaker_text_acquire(&slot, &capacity);
        if (!text)
            return;
        size_t length = param->meta_rsp.attr_length > 0 ? (size_t)param->meta_rsp.attr_length : 0;
        if (length >= capacity)
            length = capacity - 1u;
        if (param->meta_rsp.attr_text)
            memcpy(text, param->meta_rsp.attr_text, length);
        text[length] = '\0';
        /* Do not allow metadata to inject terminal control sequences. */
        for (size_t i = 0; i < length; ++i)
            if ((unsigned char)text[i] < 32u || text[i] == 127)
                text[i] = ' ';
        msg.kind = METADATA;
        msg.value = param->meta_rsp.attr_id;
        msg.payload.metadata.slot = slot;
        msg.payload.metadata.length = (uint8_t)length;
        has_text = true;
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
        (void)speaker_post(&msg);
        msg.kind = POSITION;
        msg.value = param->play_status_rsp.song_position;
        msg.extra = param->play_status_rsp.song_length;
        break;
    default:
        return;
    }
    if (!speaker_post(&msg) && has_text)
        speaker_text_release(msg.payload.metadata.slot);
}

static void tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        (void)speaker_post(
            &(speaker_event_t){.kind = TG_LINK, .value = param->conn_stat.connected});
        break;
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
        (void)speaker_post(
            &(speaker_event_t){.kind = VOLUME, .value = param->set_abs_vol.volume & 127u});
        break;
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
        if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE)
            (void)speaker_post(&(speaker_event_t){.kind = VOLUME_SUBSCRIBE});
        break;
    default:
        break;
    }
}

void speaker_avrcp_register(void) {
    ESP_ERROR_CHECK(esp_avrc_ct_register_callback(ct_cb));
    ESP_ERROR_CHECK(esp_avrc_ct_init());
    ESP_ERROR_CHECK(esp_avrc_tg_register_callback(tg_cb));
    ESP_ERROR_CHECK(esp_avrc_tg_init());
    esp_avrc_rn_evt_cap_mask_t mask = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &mask, ESP_AVRC_RN_VOLUME_CHANGE);
    ESP_ERROR_CHECK(esp_avrc_tg_set_rn_evt_cap(&mask));
}
