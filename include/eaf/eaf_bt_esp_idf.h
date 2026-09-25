#pragma once
#include <eaf/eaf_bt.h>

/* Singleton Classic A2DP sink for ESP-IDF/Bluedroid. The application owns
 * controller and Bluedroid init/enable, the GAP device name, pairing and
 * discoverability, the decode worker and the audio graph. */
typedef enum {
    EAF_BT_CONNECTED,
    EAF_BT_DISCONNECTED,
    EAF_BT_CONFIGURED,
    EAF_BT_STARTED,
    EAF_BT_SUSPENDED,
    EAF_BT_RELEASED
} eaf_bt_esp_idf_event_t;
typedef void (*eaf_bt_esp_idf_notify_t)(void *ctx, eaf_bt_esp_idf_event_t event);

/* Call once after esp_bluedroid_enable() and before connections are accepted.
 * The queue and callback context must live for the Bluetooth host lifetime.
 * Events run on the Bluedroid callback task: only post work, never block,
 * decode or mutate the graph. All streams use the chosen 44100 or 48000 Hz
 * rate and stereo PCM after decode. A failed registration cannot be retried.
 * Returns EAF_OK or a negative EAF code. */
int eaf_bt_esp_idf_register(eaf_bt_ingress_t *queue, uint32_t sample_rate,
                            eaf_bt_esp_idf_notify_t notify, void *ctx);
