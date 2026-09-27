#include <eaf/eaf_bt_esp_idf.h>
#include <esp_a2dp_api.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <stdint.h>
#include <string.h>

/* Bluedroid exposes the RTP media header separately from the audio buffer, but
 * the portable ingress expects the media-header-first layout Zephyr provides.
 * This table maps a codec to its A2DP capability advertisement and to the
 * negotiated-configuration check. ESP-IDF v6.0.2 supports a single SEP
 * (ESP_A2D_MAX_SEPS), so exactly one entry is active; adding AAC later means a
 * new descriptor plus a selection source, not a new callback path. */
typedef struct {
    esp_a2d_mct_t media_type;
    void (*fill_caps)(esp_a2d_mcc_t *mcc, uint32_t sample_rate);
    bool (*accepts)(const esp_a2d_mcc_t *mcc, uint32_t sample_rate);
} eaf_bt_codec_desc_t;

static void sbc_fill_caps(esp_a2d_mcc_t *mcc, uint32_t sample_rate) {
    *mcc = (esp_a2d_mcc_t){0};
    mcc->type = ESP_A2D_MCT_SBC;
    mcc->cie.sbc_info.samp_freq =
        sample_rate == 48000u ? ESP_A2D_SBC_CIE_SF_48K : ESP_A2D_SBC_CIE_SF_44K;
    mcc->cie.sbc_info.ch_mode =
        ESP_A2D_SBC_CIE_CH_MODE_STEREO | ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
    mcc->cie.sbc_info.block_len = ESP_A2D_SBC_CIE_BLOCK_LEN_16;
    mcc->cie.sbc_info.num_subbands = ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
    mcc->cie.sbc_info.alloc_mthd = ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
    mcc->cie.sbc_info.min_bitpool = 2;
    mcc->cie.sbc_info.max_bitpool = 53;
}

static bool sbc_accepts(const esp_a2d_mcc_t *mcc, uint32_t sample_rate) {
    if (mcc->type != ESP_A2D_MCT_SBC)
        return false;
    const esp_a2d_cie_sbc_t *sbc = &mcc->cie.sbc_info;
    uint8_t want_sf = sample_rate == 48000u ? ESP_A2D_SBC_CIE_SF_48K : ESP_A2D_SBC_CIE_SF_44K;
    return (sbc->samp_freq & want_sf) != 0 &&
           (sbc->ch_mode &
            (ESP_A2D_SBC_CIE_CH_MODE_STEREO | ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO)) != 0 &&
           sbc->block_len == ESP_A2D_SBC_CIE_BLOCK_LEN_16 &&
           sbc->num_subbands == ESP_A2D_SBC_CIE_NUM_SUBBANDS_8 &&
           sbc->alloc_mthd == ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS && sbc->min_bitpool >= 2 &&
           sbc->max_bitpool <= 53 && sbc->min_bitpool <= sbc->max_bitpool;
}

static const eaf_bt_codec_desc_t eaf_bt_codec_table[] = {
    {ESP_A2D_MCT_SBC, sbc_fill_caps, sbc_accepts},
};

static struct {
    eaf_bt_ingress_t *ingress;
    eaf_bt_esp_idf_notify_t notify;
    void *ctx;
    const eaf_bt_codec_desc_t *codec;
    uint32_t sample_rate;
    uint16_t sequence;
    bool registered, configured;
    uint32_t acknowledged;
    eaf_bt_esp_idf_status_t status;
    /* Single serialized Bluedroid producer, so one staging buffer is enough. */
    uint8_t staging[1u + EAF_BT_PACKET_BYTES];
} state;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

void eaf_bt_esp_idf_status(eaf_bt_esp_idf_status_t *status) {
    if (!status)
        return;
    portENTER_CRITICAL(&lock);
    *status = state.status;
    portEXIT_CRITICAL(&lock);
}

bool eaf_bt_esp_idf_sync(eaf_bt_esp_idf_status_t *status) {
    if (!status)
        return false;
    portENTER_CRITICAL(&lock);
    bool changed = status->generation != state.status.generation;
    if (changed) {
        if (state.ingress)
            eaf_bt_ingress_init(state.ingress);
        state.acknowledged = state.status.generation;
    }
    *status = state.status;
    portEXIT_CRITICAL(&lock);
    return changed;
}

static void emit(eaf_bt_esp_idf_event_t event) {
    if (state.notify)
        state.notify(state.ctx, event);
}

static void audio_data_cb(esp_a2d_conn_hdl_t conn_hdl, esp_a2d_audio_buff_t *audio_buf) {
    (void)conn_hdl;
    if (!audio_buf)
        return;
    portENTER_CRITICAL(&lock);
    if (state.status.streaming && state.acknowledged == state.status.generation &&
        audio_buf->data && audio_buf->number_frame && audio_buf->number_frame <= 15u &&
        audio_buf->data_len <= EAF_BT_PACKET_BYTES) {
        state.staging[0] = (uint8_t)(audio_buf->number_frame & 0x0fu);
        memcpy(&state.staging[1], audio_buf->data, audio_buf->data_len);
        (void)eaf_bt_sbc_receive(state.ingress, state.sequence++, audio_buf->timestamp,
                                 state.staging, (size_t)audio_buf->data_len + 1u);
    }
    portEXIT_CRITICAL(&lock);
    esp_a2d_audio_buff_free(audio_buf);
}

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    eaf_bt_esp_idf_event_t notification = EAF_BT_RELEASED;
    bool notify = false;
    portENTER_CRITICAL(&lock);
    switch (event) {
    case ESP_A2D_SEP_REG_STATE_EVT:
        state.status.ready = param->a2d_sep_reg_stat.reg_state == ESP_A2D_SEP_REG_SUCCESS;
        break;
    case ESP_A2D_CONNECTION_STATE_EVT:
        state.status.streaming = false;
        ++state.status.generation;
        state.status.connected = param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED;
        if (!state.status.connected)
            state.configured = false;
        state.status.connecting = param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING;
        memcpy(state.status.peer, param->conn_stat.remote_bda, sizeof(state.status.peer));
        notification = state.status.connected ? EAF_BT_CONNECTED : EAF_BT_DISCONNECTED;
        notify = true;
        break;
    case ESP_A2D_AUDIO_CFG_EVT:
        state.configured = state.codec->accepts(&param->audio_cfg.mcc, state.sample_rate);
        state.status.streaming = false;
        ++state.status.generation;
        notification = state.configured ? EAF_BT_CONFIGURED : EAF_BT_RELEASED;
        notify = true;
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        state.status.streaming = state.status.connected && state.configured &&
                                 param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        ++state.status.generation;
        notification = state.status.streaming ? EAF_BT_STARTED : EAF_BT_SUSPENDED;
        notify = true;
        break;
    default:
        break;
    }
    portEXIT_CRITICAL(&lock);
    if (notify)
        emit(notification);
}

int eaf_bt_esp_idf_register(eaf_bt_ingress_t *queue, uint32_t sample_rate,
                            eaf_bt_esp_idf_notify_t notify, void *ctx) {
    if (!queue || !notify || (sample_rate != 44100u && sample_rate != 48000u))
        return EAF_INVALID;
    if (state.registered)
        return EAF_STATE;
    state.ingress = queue;
    state.notify = notify;
    state.ctx = ctx;
    state.codec = &eaf_bt_codec_table[0];
    state.sample_rate = sample_rate;
    state.sequence = 1u;
    /* Publish the first generation under the lock so a consumer reading the
     * status can never observe a generation without the ingress pointer. */
    portENTER_CRITICAL(&lock);
    state.status.generation = 1;
    portEXIT_CRITICAL(&lock);
    if (esp_a2d_register_callback(&a2d_cb) != ESP_OK)
        return EAF_IO;
    if (esp_a2d_sink_init() != ESP_OK)
        return EAF_IO;
    esp_a2d_mcc_t mcc = {0};
    state.codec->fill_caps(&mcc, sample_rate);
    if (esp_a2d_sink_register_stream_endpoint(0, &mcc) != ESP_OK)
        return EAF_IO;
    if (esp_a2d_sink_register_audio_data_callback(audio_data_cb) != ESP_OK)
        return EAF_IO;
    state.registered = true;
    return EAF_OK;
}
