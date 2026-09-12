#include "output.h"
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>

/* dhcpv4.h requires the net_if declaration first. */
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
    if (event == NET_EVENT_WIFI_DISCONNECT_RESULT)
        atomic_store(&associated, false);
    if (event == NET_EVENT_WIFI_CONNECT_RESULT && cb->info &&
        cb->info_length >= sizeof(struct wifi_status)) {
        const struct wifi_status *status = cb->info;
        atomic_store(&associated, status->status == 0);
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
    if (!online())
        return EAF_TIMEOUT;
    char address[NET_IPV4_ADDR_LEN];
    struct in_addr *ip = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
    if (!ip)
        return EAF_IO;
    printk("Wi-Fi IPv4: %s\n", net_addr_ntop(AF_INET, ip, address, sizeof(address)));
    return EAF_OK;
}
int main(void) {
    const char *ssid = board_wifi_ssid(), *password = board_wifi_password();
    size_t ssid_length = strlen(ssid), password_length = strlen(password);
    if (!ssid_length || ssid_length > 32 || password_length < 8 || password_length > 63) {
        printk("Set a 2.4 GHz WPA2 SSID/passphrase in credentials.local.h and rebuild\n");
        return 1;
    }
    iface = net_if_get_first_wifi();
    if (!iface) {
        printk("No Wi-Fi interface\n");
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
        printk("Output initialization failed\n");
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
    printk("EAF LMS board: player %02x:%02x:%02x:%02x:%02x:%02x, server %s:3483\n", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5], CONFIG_EAF_BOARD_SERVER);
    for (;;) {
        int rc = online() ? EAF_OK : connect_wifi();
        if (!rc)
            rc = eaf_lms_client_connect(&client, sys_be32_to_cpu(server.s_addr), 3483, mac);
        if (!rc)
            printk("LMS connected; select this player's MAC in the server UI\n");
        int64_t report = 0;
        while (!rc && online() && !board_output_failed()) {
            rc = eaf_lms_client_step(&client);
            int64_t now = k_uptime_get();
            eaf_lms_playback_t snapshot;
            if (!rc && !client.wait_cont && !client.wait_start && now >= report &&
                board_output_snapshot(&snapshot)) {
                rc = eaf_lms_client_report_playback(&client, &snapshot);
                report = now + 1000;
            }
            k_sleep(K_MSEC(2));
        }
        eaf_lms_client_close(&client);
        if (board_output_failed()) {
            printk("Output failure: playback stopped; inspect serial log and reset\n");
            return 1;
        }
        printk("Connection ended (%d); retry in 5 seconds\n", rc);
        if (!online()) {
            (void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
            net_dhcpv4_stop(iface);
        }
        k_sleep(K_SECONDS(5));
    }
    return 0;
}
