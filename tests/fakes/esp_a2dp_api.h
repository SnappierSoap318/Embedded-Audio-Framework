#pragma once
/* Host mirror of the ESP-IDF A2DP sink API surface used by
 * hal/esp_idf/bt_a2dp_esp_idf.c. Field order and bit widths match the real
 * esp_a2dp_api.h so the binding is exercised against a faithful layout. */
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef uint8_t esp_bd_addr_t[6];
typedef uint16_t esp_a2d_conn_hdl_t;
typedef uint8_t esp_a2d_mct_t;

#define ESP_A2D_MCT_SBC 0x00

#define ESP_A2D_SBC_CIE_SF_16K (0x8)
#define ESP_A2D_SBC_CIE_SF_32K (0x4)
#define ESP_A2D_SBC_CIE_SF_44K (0x2)
#define ESP_A2D_SBC_CIE_SF_48K (0x1)
#define ESP_A2D_SBC_CIE_CH_MODE_MONO (0x8)
#define ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL (0x4)
#define ESP_A2D_SBC_CIE_CH_MODE_STEREO (0x2)
#define ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO (0x1)
#define ESP_A2D_SBC_CIE_BLOCK_LEN_4 (0x8)
#define ESP_A2D_SBC_CIE_BLOCK_LEN_8 (0x4)
#define ESP_A2D_SBC_CIE_BLOCK_LEN_12 (0x2)
#define ESP_A2D_SBC_CIE_BLOCK_LEN_16 (0x1)
#define ESP_A2D_SBC_CIE_NUM_SUBBANDS_4 (0x2)
#define ESP_A2D_SBC_CIE_NUM_SUBBANDS_8 (0x1)
#define ESP_A2D_SBC_CIE_ALLOC_MTHD_SNR (0x2)
#define ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS (0x1)

typedef struct {
    uint8_t ch_mode : 4;
    uint8_t samp_freq : 4;
    uint8_t alloc_mthd : 2;
    uint8_t num_subbands : 2;
    uint8_t block_len : 4;
    uint8_t min_bitpool;
    uint8_t max_bitpool;
} __attribute__((packed)) esp_a2d_cie_sbc_t;

typedef struct {
    esp_a2d_mct_t type;
    union {
        esp_a2d_cie_sbc_t sbc_info;
    } cie;
} __attribute__((packed)) esp_a2d_mcc_t;

typedef enum {
    ESP_A2D_CONNECTION_STATE_DISCONNECTED = 0,
    ESP_A2D_CONNECTION_STATE_CONNECTING,
    ESP_A2D_CONNECTION_STATE_CONNECTED,
    ESP_A2D_CONNECTION_STATE_DISCONNECTING
} esp_a2d_connection_state_t;

typedef enum { ESP_A2D_AUDIO_STATE_SUSPEND = 0, ESP_A2D_AUDIO_STATE_STARTED } esp_a2d_audio_state_t;

typedef enum {
    ESP_A2D_SEP_REG_SUCCESS = 0,
    ESP_A2D_SEP_REG_FAIL,
    ESP_A2D_SEP_REG_UNSUPPORTED,
    ESP_A2D_SEP_REG_INVALID_STATE
} esp_a2d_sep_reg_state_t;

typedef enum {
    ESP_A2D_CONNECTION_STATE_EVT = 0,
    ESP_A2D_AUDIO_STATE_EVT,
    ESP_A2D_AUDIO_CFG_EVT,
    ESP_A2D_MEDIA_CTRL_ACK_EVT,
    ESP_A2D_PROF_STATE_EVT,
    ESP_A2D_SEP_REG_STATE_EVT
} esp_a2d_cb_event_t;

typedef struct {
    uint16_t buff_size;
    uint16_t number_frame;
    uint32_t timestamp;
    uint16_t data_len;
    uint8_t *data;
} esp_a2d_audio_buff_t;

typedef union {
    struct {
        esp_a2d_connection_state_t state;
        esp_bd_addr_t remote_bda;
        esp_a2d_conn_hdl_t conn_hdl;
        uint16_t audio_mtu;
        int disc_rsn;
    } conn_stat;
    struct {
        esp_a2d_audio_state_t state;
        esp_bd_addr_t remote_bda;
        esp_a2d_conn_hdl_t conn_hdl;
    } audio_stat;
    struct {
        esp_bd_addr_t remote_bda;
        esp_a2d_conn_hdl_t conn_hdl;
        esp_a2d_mcc_t mcc;
    } audio_cfg;
    struct {
        uint8_t seid;
        esp_a2d_sep_reg_state_t reg_state;
    } a2d_sep_reg_stat;
} esp_a2d_cb_param_t;

typedef void (*esp_a2d_cb_t)(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
typedef void (*esp_a2d_sink_audio_data_cb_t)(esp_a2d_conn_hdl_t conn_hdl,
                                             esp_a2d_audio_buff_t *audio_buf);

esp_err_t esp_a2d_register_callback(esp_a2d_cb_t callback);
esp_err_t esp_a2d_sink_init(void);
esp_err_t esp_a2d_sink_register_stream_endpoint(uint8_t seid, esp_a2d_mcc_t *mcc);
esp_err_t esp_a2d_sink_register_audio_data_callback(esp_a2d_sink_audio_data_cb_t callback);
esp_err_t esp_a2d_sink_disconnect(esp_bd_addr_t bd_addr);
void esp_a2d_audio_buff_free(esp_a2d_audio_buff_t *audio_buf);
