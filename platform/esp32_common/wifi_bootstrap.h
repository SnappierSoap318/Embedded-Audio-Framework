#pragma once
#include <eaf/eaf_types.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

/* Runtime Wi-Fi association state, owned by the application's main thread. */
typedef struct {
    struct net_if *iface;
    atomic_t associated;
    struct net_mgmt_event_callback events;
    struct wifi_connect_req_params connection;
} board_wifi_t;

/* Credentials compiled from the private credentials.h (see credentials.example.h). */
const char *board_wifi_ssid(void);
const char *board_wifi_password(void);

/* Validate credentials, find the Wi-Fi interface and arm association events. */
int board_wifi_bind(board_wifi_t *wifi);
/* Start DHCP and connect, waiting up to 30 s for a preferred IPv4 address. */
int board_wifi_connect(board_wifi_t *wifi);
bool board_wifi_online(const board_wifi_t *wifi);
void board_wifi_health(const board_wifi_t *wifi);
