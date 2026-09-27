#include "board_runtime.h"
#include "diagnostics.h"
#include "output.h"
#include "wifi_bootstrap.h"
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>

/* dhcpv4.h requires the net_if declaration first. */
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/byteorder.h>
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT), "Qualify Wi-Fi alone before enabling Bluetooth");
BUILD_ASSERT(!IS_ENABLED(CONFIG_EAF_BOARD_USE_PSRAM) || IS_ENABLED(CONFIG_ESP_SPIRAM),
             "PSRAM reservoir requires CONFIG_ESP_SPIRAM");
static eaf_lms_client_t client;
static board_wifi_t wifi;

int main(void) {
    if (CONFIG_EAF_BOARD_MAIN_CPU >= 0)
        (void)board_cpu_pin(k_current_get(), CONFIG_EAF_BOARD_MAIN_CPU);
    board_diagnostics_start();
    if (board_wifi_bind(&wifi) != EAF_OK)
        return 1;
    struct in_addr server;
    if (net_addr_pton(AF_INET, CONFIG_EAF_BOARD_SERVER, &server))
        return 1;
    const struct net_linkaddr *link = net_if_get_link_addr(wifi.iface);
    if (!link || link->len != 6)
        return 1;
    uint8_t mac[6];
    memcpy(mac, link->addr, sizeof(mac));
    if (board_output_init()) {
        board_log("Output initialization failed");
        return 1;
    }
    eaf_lms_callbacks_t cb = board_output_callbacks();
    if (eaf_lms_client_init(&client, &cb))
        return 1;
    board_log("EAF LMS board: player %02x:%02x:%02x:%02x:%02x:%02x, server %s:3483", mac[0], mac[1],
              mac[2], mac[3], mac[4], mac[5], CONFIG_EAF_BOARD_SERVER);
    for (;;) {
        int rc = board_wifi_online(&wifi) ? EAF_OK : board_wifi_connect(&wifi);
        if (!rc) {
            rc = eaf_lms_client_connect(&client, sys_be32_to_cpu(server.s_addr), 3483, mac);
            if (rc)
                board_log("LMS connect failed rc=%d server=%s:3483", rc, CONFIG_EAF_BOARD_SERVER);
        }
        if (!rc)
            board_log("LMS connected; select this player's MAC in the server UI");
        int64_t report = 0, diagnostic = 0, previous_diagnostic = k_uptime_get();
        uint64_t previous_bytes = client.diagnostics.http_bytes;
        bool eof_reported = false;
        while (!rc && board_wifi_online(&wifi) && !board_output_failed()) {
            eaf_lms_pump_result_t pump;
            rc = eaf_lms_client_pump(&client, 32, 1000, &pump);
            if (rc) {
                const eaf_lms_diagnostics_t *d = &client.diagnostics;
                board_log("LMS failure rc=%d stage=%u opcode=%02x%02x%02x%02x", d->first_error,
                          (unsigned)d->error_stage, d->error_opcode[0], d->error_opcode[1],
                          d->error_opcode[2], d->error_opcode[3]);
            }
            int64_t now = k_uptime_get();
            eaf_lms_playback_t snapshot;
            if (now >= diagnostic) {
                bool active = board_output_snapshot(&snapshot);
                const eaf_lms_diagnostics_t *d = &client.diagnostics;
                uint64_t span = (uint64_t)(now - previous_diagnostic);
                uint64_t rate = span ? (d->http_bytes - previous_bytes) * 1000u / span : 0;
                board_log_memory("RX=%llu B/s total=%llu again=%u backpressure=%u budget=%u",
                                 (unsigned long long)rate, (unsigned long long)d->http_bytes,
                                 d->recv_again, d->backpressure, d->budget_yields);
                board_log_memory(
                    "Output queued=%u min_frames=%u played_ms=%u underruns=%u failed=%u",
                    active ? snapshot.queued_bytes : 0u, board_output_queue_min(),
                    active ? snapshot.elapsed_ms : 0u, board_output_underruns(),
                    board_output_failed() ? 1u : 0u);
                board_log_memory("Worker flags=%u calls=%u HTTP=%u eof=%u gates=%u/%u",
                                 board_output_flags(), board_output_process_calls(),
                                 client.http.open ? 1u : 0u, client.input_eof ? 1u : 0u,
                                 client.wait_cont ? 1u : 0u, client.wait_start ? 1u : 0u);
                board_wifi_health(&wifi);
                previous_bytes = d->http_bytes;
                previous_diagnostic = now;
                diagnostic = now + 5000;
            }
            if (!rc && !client.wait_cont && !client.wait_start && now >= report &&
                board_output_snapshot(&snapshot)) {
                rc = eaf_lms_client_report_playback(&client, &snapshot);
                report = now + 1000;
            }
            /* One bounded record of what the server actually sent this stream. */
            if (!client.input_eof) {
                eof_reported = false;
            } else if (!eof_reported) {
                const eaf_lms_diagnostics_t *d = &client.diagnostics;
                board_log("HTTP status=%u len=%lu known=%u type=%s bytes=%llu pcm=%llu",
                          (unsigned)d->http_status, (unsigned long)d->http_content_length,
                          d->http_length_known ? 1u : 0u,
                          d->http_content_type[0] ? d->http_content_type : "(none)",
                          (unsigned long long)d->http_bytes, (unsigned long long)d->pcm_frames);
                eof_reported = true;
            }
            /* Yield to lower-priority diagnostics even during sustained intake.
               Idle/backpressure polls remain bounded; no fixed delay per recv. */
            k_sleep(pump.budget_exhausted ? K_TICKS(1) : K_MSEC(2));
        }
        eaf_lms_client_close(&client);
        if (board_output_failed()) {
            board_log("Output failure: playback stopped; inspect serial log and reset");
            return 1;
        }
        board_log("Connection ended (%d); retry in 5 seconds", rc);
        if (!board_wifi_online(&wifi)) {
            (void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, wifi.iface, NULL, 0);
            net_dhcpv4_stop(wifi.iface);
        }
        k_sleep(K_SECONDS(5));
    }
    return 0;
}
