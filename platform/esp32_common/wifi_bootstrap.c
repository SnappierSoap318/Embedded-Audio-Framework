#include "wifi_bootstrap.h"
#include "board_runtime.h"
#include "credentials.h"
#include "diagnostics.h"
#include <string.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>

/* Private strings are never printed or passed to Kconfig. */
const char *board_wifi_ssid(void) {
    return EAF_WIFI_SSID;
}
const char *board_wifi_password(void) {
    return EAF_WIFI_PASSWORD;
}

static void wifi_event(struct net_mgmt_event_callback *cb, uint64_t event,
                       struct net_if *event_iface) {
    board_wifi_t *wifi = CONTAINER_OF(cb, board_wifi_t, events);
    if (event_iface != wifi->iface)
        return;
    if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        atomic_set(&wifi->associated, 0);
        board_log("Wi-Fi disconnected");
    }
    if (event == NET_EVENT_WIFI_CONNECT_RESULT && cb->info &&
        cb->info_length >= sizeof(struct wifi_status)) {
        const struct wifi_status *status = cb->info;
        atomic_set(&wifi->associated, status->status == 0);
        board_log("Wi-Fi association result: %d", status->status);
    }
}

bool board_wifi_online(const board_wifi_t *wifi) {
    return wifi && atomic_get(&wifi->associated) &&
           net_if_ipv4_get_global_addr(wifi->iface, NET_ADDR_PREFERRED) != NULL;
}

int board_wifi_bind(board_wifi_t *wifi) {
    if (!wifi)
        return EAF_INVALID;
    const char *ssid = board_wifi_ssid();
    const char *password = board_wifi_password();
    size_t ssid_length = strlen(ssid), password_length = strlen(password);
    if (!ssid_length || ssid_length > 32 || password_length < 8 || password_length > 63) {
        board_log("Set a 2.4 GHz WPA2 SSID/passphrase in credentials.local.h and rebuild");
        return EAF_INVALID;
    }
    struct net_if *iface = net_if_get_first_wifi();
    if (!iface) {
        board_log("No Wi-Fi interface");
        return EAF_IO;
    }
    wifi->iface = iface;
    atomic_set(&wifi->associated, 0);
    net_mgmt_init_event_callback(&wifi->events, wifi_event,
                                 NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&wifi->events);
    wifi->connection = (struct wifi_connect_req_params){.ssid = (const uint8_t *)ssid,
                                                        .ssid_length = (uint8_t)ssid_length,
                                                        .psk = (const uint8_t *)password,
                                                        .psk_length = (uint8_t)password_length,
                                                        .security = WIFI_SECURITY_TYPE_PSK,
                                                        .channel = WIFI_CHANNEL_ANY,
                                                        .band = WIFI_FREQ_BAND_2_4_GHZ,
                                                        .bandwidth = WIFI_FREQ_BANDWIDTH_20MHZ,
                                                        .timeout = 20};
    return EAF_OK;
}

int board_wifi_connect(board_wifi_t *wifi) {
    if (!wifi || !wifi->iface)
        return EAF_INVALID;
    atomic_set(&wifi->associated, 0);
    net_dhcpv4_start(wifi->iface);
    int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, wifi->iface, &wifi->connection,
                      sizeof(wifi->connection));
    if (rc)
        return rc;
    int64_t deadline = k_uptime_get() + 30000;
    while (!board_wifi_online(wifi) && k_uptime_get() < deadline)
        k_sleep(K_MSEC(100));
    if (!board_wifi_online(wifi)) {
        board_log("Wi-Fi timeout: associated=%u, IPv4=%u", atomic_get(&wifi->associated) ? 1u : 0u,
                  net_if_ipv4_get_global_addr(wifi->iface, NET_ADDR_PREFERRED) ? 1u : 0u);
        return EAF_TIMEOUT;
    }
    char address[NET_IPV4_ADDR_LEN];
    struct in_addr *ip = net_if_ipv4_get_global_addr(wifi->iface, NET_ADDR_PREFERRED);
    if (!ip)
        return EAF_IO;
    board_log("Wi-Fi IPv4: %s", net_addr_ntop(AF_INET, ip, address, sizeof(address)));
    board_wifi_power_save_off();
    return EAF_OK;
}

void board_wifi_health(const board_wifi_t *wifi) {
    if (!wifi || !wifi->iface)
        return;
    struct wifi_iface_status status;
    if (!net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, wifi->iface, &status, sizeof(status)))
        board_log("Wi-Fi rssi=%d dtim=%u beacon=%u", status.rssi, (unsigned)status.dtim_period,
                  (unsigned)status.beacon_interval);
}
