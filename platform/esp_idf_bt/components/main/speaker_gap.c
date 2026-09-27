#include "speaker_internal.h"
#include <eaf/eaf_bt_esp_idf.h>
#include <esp_bt.h>
#include <esp_gap_bt_api.h>
#include <sdkconfig.h>
#include <string.h>

bool speaker_pairing_allowed(const uint8_t *peer) {
    portENTER_CRITICAL(&speaker_lock);
    bool allowed = speaker_allow_pairing;
    for (int i = 0; i < speaker_bond_count; ++i)
        allowed |= memcmp(speaker_bonds[i], peer, 6) == 0;
    portEXIT_CRITICAL(&speaker_lock);
    return allowed;
}

void speaker_refresh_bonds(void) {
    esp_bd_addr_t list[16];
    int count = 16;
    esp_err_t err = esp_bt_gap_get_bond_device_list(&count, list);
    if (err != ESP_OK) {
        speaker_check("read bonds", err);
        return;
    }
    portENTER_CRITICAL(&speaker_lock);
    speaker_bond_count = count;
    memcpy(speaker_bonds, list, (size_t)count * sizeof(list[0]));
    portEXIT_CRITICAL(&speaker_lock);
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
    case ESP_BT_GAP_CFM_REQ_EVT:
        /* NoInputNoOutput has no display to compare; a headless speaker always
         * accepts Just Works so a source never has to type a code. */
        speaker_check("confirm pairing", esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true));
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
        bool accept = !param->pin_req.min_16_digit && speaker_pairing_allowed(param->pin_req.bda);
        speaker_check("legacy PIN", esp_bt_gap_pin_reply(param->pin_req.bda, accept, 4, pin));
        break;
    }
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        speaker_event_t msg = {.kind = AUTH, .value = param->auth_cmpl.stat};
        memcpy(msg.payload.peer, param->auth_cmpl.bda, 6);
        (void)speaker_post(&msg);
        break;
    }
    case ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT:
        (void)speaker_post(
            &(speaker_event_t){.kind = BOND_REMOVED, .value = param->remove_bond_dev_cmpl.status});
        break;
    default:
        break;
    }
}

/* Drop the remembered peer, remove its bond and become discoverable. Used by the
 * explicit forget command and when a peer that unpaired on its side rejects our
 * link key, so a stale bond cannot keep the speaker invisible. */
void speaker_begin_forget(void) {
    speaker.forgetting = true;
    speaker.have_peer = false;
    speaker.status.pairing = false;
    speaker_disconnect();
    speaker_scan_mode();
    speaker_nvs_forget_peer();
}

void speaker_gap_tick(int64_t now) {
    eaf_bt_esp_idf_status_t link;
    eaf_bt_esp_idf_status(&link);
    if (speaker.forgetting && !link.connected && !link.connecting) {
        /* Remove one at a time; wait for the asynchronous list to change before
         * requesting the next. Polling also recovers a lost completion event. */
        static int64_t remove_at;
        if (esp_bt_gap_get_bond_device_num() == 0) {
            speaker_refresh_bonds();
            speaker.forgetting = false;
            speaker_start_pairing();
        } else if (now >= remove_at) {
            esp_bd_addr_t peer[1];
            int count = 1;
            if (esp_bt_gap_get_bond_device_list(&count, peer) == ESP_OK && count)
                speaker_check("remove bond", esp_bt_gap_remove_bond_device(peer[0]));
            remove_at = now + 2000;
        }
    }
}

void speaker_gap_register(void) {
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_cb));
    esp_bt_io_cap_t capability = ESP_BT_IO_CAP_NONE;
    ESP_ERROR_CHECK(
        esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &capability, sizeof(capability)));
    /* Legacy peers that cannot do SSP get the deterministic speaker PIN 0000. */
    esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
    ESP_ERROR_CHECK(esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin));
    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(CONFIG_EAF_BT_DEVICE_NAME));
    /* Advertise as a headless Audio/Video loudspeaker: major Audio/Video,
     * minor loudspeaker (5), with the Audio and Rendering service bits. */
    esp_bt_cod_t cod = {0};
    cod.major = ESP_BT_COD_MAJOR_DEV_AV;
    cod.minor = 0x05;
    cod.service = ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_RENDERING;
    speaker_check("class of device", esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL));
}
