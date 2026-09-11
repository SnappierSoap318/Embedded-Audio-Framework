#pragma once
#include <eaf/eaf_bt.h>

/* Singleton Classic A2DP SBC sink. The application owns bt_enable(), SDP,
 * pairing/discoverability, the decoder worker and the audio graph. */
typedef enum {
    EAF_BT_CONFIGURED,
    EAF_BT_STARTED,
    EAF_BT_SUSPENDED,
    EAF_BT_RELEASED
} eaf_bt_zephyr_event_t;
typedef void (*eaf_bt_zephyr_notify_t)(void *ctx, eaf_bt_zephyr_event_t event);
/* Call once after bt_enable(), before allowing connections. Queue and callback
 * context must live for the Bluetooth host lifetime. Events run on the BT
 * callback thread: only post work, never block, decode or mutate the graph.
 * All streams use the chosen 44100 or 48000 Hz rate, stereo PCM after decode.
 * A failed registration cannot be retried (Zephyr provides no unregister API).
 * Returns zero or a negative errno. */
int eaf_bt_zephyr_register(eaf_bt_ingress_t *queue, uint32_t sample_rate,
                           eaf_bt_zephyr_notify_t notify, void *ctx);
