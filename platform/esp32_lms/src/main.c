#include "diagnostics.h"
#include "output.h"
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>

/* dhcpv4.h requires the net_if declaration first. */
#if defined(CONFIG_WIFI_ESP32)
#include <esp_wifi.h>
#endif
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/byteorder.h>
BUILD_ASSERT(!IS_ENABLED(CONFIG_ESP_SPIRAM), "WROOM application requires internal RAM");
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT), "Qualify Wi-Fi alone before enabling Bluetooth");
static eaf_lms_client_t client;
static struct net_if *iface;
static struct net_mgmt_event_callback events;
static atomic_bool associated;
static struct wifi_connect_req_params connection;
static void wifi_event(struct net_mgmt_event_callback *cb, uint64_t event,
                       struct net_if *event_iface) {
    if (event_iface != iface)
        return;
    if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        atomic_store(&associated, false);
        board_log("Wi-Fi disconnected");
    }
    if (event == NET_EVENT_WIFI_CONNECT_RESULT && cb->info &&
        cb->info_length >= sizeof(struct wifi_status)) {
        const struct wifi_status *status = cb->info;
        atomic_store(&associated, status->status == 0);
        board_log("Wi-Fi association result: %d", status->status);
    }
}
static bool online(void) {
    return atomic_load(&associated) &&
           net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL;
}
static int connect_wifi(void) {
    atomic_store(&associated, false);
    net_dhcpv4_start(iface);
    int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &connection, sizeof(connection));
    if (rc)
        return rc;
    int64_t deadline = k_uptime_get() + 30000;
    while (!online() && k_uptime_get() < deadline)
        k_sleep(K_MSEC(100));
    if (!online()) {
        board_log("Wi-Fi timeout: associated=%u, IPv4=%u", atomic_load(&associated) ? 1u : 0u,
                  net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) ? 1u : 0u);
        return EAF_TIMEOUT;
    }
    char address[NET_IPV4_ADDR_LEN];
    struct in_addr *ip = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
    if (!ip)
        return EAF_IO;
    board_log("Wi-Fi IPv4: %s", net_addr_ntop(AF_INET, ip, address, sizeof(address)));
#if defined(CONFIG_WIFI_ESP32) && defined(CONFIG_EAF_BOARD_WIFI_PS_NONE)
    board_log("Wi-Fi power save off rc=%d", (int)esp_wifi_set_ps(WIFI_PS_NONE));
#endif
    return EAF_OK;
}
static void wifi_health(void) {
    struct wifi_iface_status status;
    if (!net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status, sizeof(status)))
        board_log_memory("Wi-Fi rssi=%d dtim=%u beacon=%u", status.rssi,
                         (unsigned)status.dtim_period, (unsigned)status.beacon_interval);
}
int main(void) {
#if defined(CONFIG_SCHED_CPU_MASK) && CONFIG_EAF_BOARD_MAIN_CPU >= 0
    (void)k_thread_cpu_pin(k_current_get(), CONFIG_EAF_BOARD_MAIN_CPU);
#endif
    board_diagnostics_start();
    const char *ssid = board_wifi_ssid(), *password = board_wifi_password();
    size_t ssid_length = strlen(ssid), password_length = strlen(password);
    if (!ssid_length || ssid_length > 32 || password_length < 8 || password_length > 63) {
        board_log("Set a 2.4 GHz WPA2 SSID/passphrase in credentials.local.h and rebuild");
        return 1;
    }
    iface = net_if_get_first_wifi();
    if (!iface) {
        board_log("No Wi-Fi interface");
        return 1;
    }
    struct in_addr server;
    if (net_addr_pton(AF_INET, CONFIG_EAF_BOARD_SERVER, &server))
        return 1;
    const struct net_linkaddr *link = net_if_get_link_addr(iface);
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
    net_mgmt_init_event_callback(&events, wifi_event,
                                 NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&events);
    connection = (struct wifi_connect_req_params){.ssid = (const uint8_t *)ssid,
                                                  .ssid_length = (uint8_t)ssid_length,
                                                  .psk = (const uint8_t *)password,
                                                  .psk_length = (uint8_t)password_length,
                                                  .security = WIFI_SECURITY_TYPE_PSK,
                                                  .channel = WIFI_CHANNEL_ANY,
                                                  .band = WIFI_FREQ_BAND_2_4_GHZ,
                                                  .bandwidth = WIFI_FREQ_BANDWIDTH_20MHZ,
                                                  .timeout = 20};
    board_log("EAF LMS board: player %02x:%02x:%02x:%02x:%02x:%02x, server %s:3483", mac[0], mac[1],
              mac[2], mac[3], mac[4], mac[5], CONFIG_EAF_BOARD_SERVER);
    for (;;) {
        int rc = online() ? EAF_OK : connect_wifi();
        if (!rc) {
            rc = eaf_lms_client_connect(&client, sys_be32_to_cpu(server.s_addr), 3483, mac);
            if (rc)
                board_log("LMS connect failed rc=%d server=%s:3483", rc, CONFIG_EAF_BOARD_SERVER);
        }
        if (!rc)
            board_log("LMS connected; select this player's MAC in the server UI");
        int64_t report = 0, diagnostic = 0, previous_diagnostic = k_uptime_get();
        uint64_t previous_bytes = client.diagnostics.http_bytes;
        while (!rc && online() && !board_output_failed()) {
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
                wifi_health();
                previous_bytes = d->http_bytes;
                previous_diagnostic = now;
                diagnostic = now + 5000;
            }
            if (!rc && !client.wait_cont && !client.wait_start && now >= report &&
                board_output_snapshot(&snapshot)) {
                rc = eaf_lms_client_report_playback(&client, &snapshot);
                report = now + 1000;
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
        if (!online()) {
            (void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
            net_dhcpv4_stop(iface);
        }
        k_sleep(K_SECONDS(5));
    }
    return 0;
}
