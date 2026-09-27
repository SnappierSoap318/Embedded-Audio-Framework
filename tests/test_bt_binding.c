#include "check.h"
#include <eaf/eaf_bt_esp_idf.h>
#include <esp_a2dp_api.h>
#include <string.h>

/* Captured Bluedroid registrations, defined before the binding is included. */
static esp_a2d_cb_t captured_event_cb;
static esp_a2d_sink_audio_data_cb_t captured_audio_cb;
static esp_a2d_mcc_t registered_mcc;
static int endpoint_registrations;

esp_err_t esp_a2d_register_callback(esp_a2d_cb_t callback) {
    captured_event_cb = callback;
    return ESP_OK;
}
esp_err_t esp_a2d_sink_init(void) {
    return ESP_OK;
}
esp_err_t esp_a2d_sink_register_stream_endpoint(uint8_t seid, esp_a2d_mcc_t *mcc) {
    (void)seid;
    registered_mcc = *mcc;
    ++endpoint_registrations;
    return ESP_OK;
}
esp_err_t esp_a2d_sink_register_audio_data_callback(esp_a2d_sink_audio_data_cb_t callback) {
    captured_audio_cb = callback;
    return ESP_OK;
}
void esp_a2d_audio_buff_free(esp_a2d_audio_buff_t *audio_buf) {
    (void)audio_buf;
}

// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../hal/esp_idf/bt_a2dp_esp_idf.c"

static eaf_bt_ingress_t queue;
static int notifications;
static void notify(void *ctx, eaf_bt_esp_idf_event_t event) {
    (void)ctx;
    (void)event;
    ++notifications;
}

static void connect(const uint8_t *peer, esp_a2d_connection_state_t link) {
    esp_a2d_cb_param_t param;
    memset(&param, 0, sizeof(param));
    param.conn_stat.state = link;
    memcpy(param.conn_stat.remote_bda, peer, 6);
    captured_event_cb(ESP_A2D_CONNECTION_STATE_EVT, &param);
}

static void configure(bool matching) {
    esp_a2d_cb_param_t param;
    memset(&param, 0, sizeof(param));
    param.audio_cfg.mcc = registered_mcc;
    if (!matching)
        param.audio_cfg.mcc.cie.sbc_info.samp_freq = ESP_A2D_SBC_CIE_SF_48K;
    captured_event_cb(ESP_A2D_AUDIO_CFG_EVT, &param);
}

static void audio_state(esp_a2d_audio_state_t media) {
    esp_a2d_cb_param_t param;
    memset(&param, 0, sizeof(param));
    param.audio_stat.state = media;
    captured_event_cb(ESP_A2D_AUDIO_STATE_EVT, &param);
}

static void status(eaf_bt_esp_idf_status_t *out) {
    eaf_bt_esp_idf_status(out);
}

static void deliver(uint16_t frames, uint16_t length, uint32_t timestamp) {
    static uint8_t payload[EAF_BT_PACKET_BYTES];
    for (size_t i = 0; i < sizeof(payload); ++i)
        payload[i] = (uint8_t)i;
    esp_a2d_audio_buff_t buff;
    memset(&buff, 0, sizeof(buff));
    buff.number_frame = frames;
    buff.data = payload;
    buff.data_len = length;
    buff.timestamp = timestamp;
    captured_audio_cb(1, &buff);
}

int main(void) {
    const uint8_t peer[6] = {1, 2, 3, 4, 5, 6};
    eaf_bt_esp_idf_status_t st;

    CHECK(eaf_bt_esp_idf_register(&queue, 44100, notify, NULL) == EAF_OK);
    CHECK(eaf_bt_esp_idf_register(&queue, 44100, notify, NULL) == EAF_STATE);
    CHECK(captured_event_cb && captured_audio_cb && endpoint_registrations == 1);
    CHECK(registered_mcc.type == ESP_A2D_MCT_SBC);
    CHECK(registered_mcc.cie.sbc_info.samp_freq == ESP_A2D_SBC_CIE_SF_44K);
    CHECK(registered_mcc.cie.sbc_info.block_len == ESP_A2D_SBC_CIE_BLOCK_LEN_16);
    CHECK(registered_mcc.cie.sbc_info.num_subbands == ESP_A2D_SBC_CIE_NUM_SUBBANDS_8);
    CHECK(registered_mcc.cie.sbc_info.alloc_mthd == ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS);
    CHECK(registered_mcc.cie.sbc_info.min_bitpool == 2 &&
          registered_mcc.cie.sbc_info.max_bitpool == 53);

    status(&st);
    CHECK(!st.ready && !st.connected && !st.streaming);

    esp_a2d_cb_param_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.a2d_sep_reg_stat.reg_state = ESP_A2D_SEP_REG_SUCCESS;
    captured_event_cb(ESP_A2D_SEP_REG_STATE_EVT, &evt);
    status(&st);
    CHECK(st.ready);

    /* Normal connect -> configure -> start delivers media. */
    connect(peer, ESP_A2D_CONNECTION_STATE_CONNECTED);
    status(&st);
    CHECK(st.connected && !st.streaming && memcmp(st.peer, peer, 6) == 0);
    configure(true);
    audio_state(ESP_A2D_AUDIO_STATE_STARTED);
    status(&st);
    CHECK(st.streaming);

    eaf_bt_packet_t packet;
    deliver(4, 9, 111);
    CHECK(eaf_bt_ingress_pop(&queue, &packet));
    CHECK(packet.frame_count == 4 && packet.length == 9 && packet.timestamp == 111);
    CHECK(packet.data[0] == (uint8_t)0 && packet.data[7] == (uint8_t)7);

    /* Suspend gates media; a second start resumes without reconfiguring. */
    audio_state(ESP_A2D_AUDIO_STATE_SUSPEND);
    status(&st);
    CHECK(!st.streaming);
    deliver(4, 9, 222);
    CHECK(!eaf_bt_ingress_pop(&queue, &packet));
    audio_state(ESP_A2D_AUDIO_STATE_STARTED);
    status(&st);
    CHECK(st.streaming);
    deliver(4, 9, 333);
    CHECK(eaf_bt_ingress_pop(&queue, &packet));
    CHECK(packet.timestamp == 333);

    /* Malformed media (zero frames, overlong payload) is dropped by the ingress. */
    deliver(0, 9, 444);
    deliver(4, (uint16_t)(EAF_BT_PACKET_BYTES + 1), 555);
    CHECK(!eaf_bt_ingress_pop(&queue, &packet));

    /* Codec-config acceptance must not gate streaming: a rejected config still
     * starts the stream (the decoder drops anything undecodable). */
    configure(false);
    audio_state(ESP_A2D_AUDIO_STATE_STARTED);
    status(&st);
    CHECK(st.streaming);
    deliver(4, 9, 666);
    CHECK(eaf_bt_ingress_pop(&queue, &packet));

    /* Disconnect clears stream state. */
    connect(peer, ESP_A2D_CONNECTION_STATE_DISCONNECTED);
    status(&st);
    CHECK(!st.connected && !st.streaming);

    /* A start after reconnect streams again without a fresh config event. */
    connect(peer, ESP_A2D_CONNECTION_STATE_CONNECTED);
    audio_state(ESP_A2D_AUDIO_STATE_STARTED);
    status(&st);
    CHECK(st.connected && st.streaming);

    CHECK(notifications > 0);
    return 0;
}
