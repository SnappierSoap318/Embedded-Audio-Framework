#include "board_config.h"
#include <board_output.h>
#include <eaf/eaf_hal.h>
#include <eaf/eaf_sendspin_client.h>
#include <eaf/eaf_sendspin_player.h>
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

BUILD_ASSERT(!IS_ENABLED(CONFIG_BT), "Qualify Wi-Fi alone before enabling Bluetooth");

static eaf_sendspin_client_t client;
static eaf_sendspin_player_t player;
static struct net_if *iface;
static struct net_mgmt_event_callback events;
static atomic_bool associated;
static struct wifi_connect_req_params connection;
static atomic_t chunks;

static void board_log(const char *message) {
    printk("%s\n", message);
}

static uint32_t board_write(void *ctx, const int32_t *samples, uint32_t frames) {
    (void)ctx;
    return eaf_board_output_write(samples, frames);
}
static void on_ready(void *ctx, const eaf_sendspin_server_hello_t *hello) {
    (void)ctx;
    printk("Sendspin ready: %.*s player=%d\n", (int)hello->name_length, hello->name,
           (int)hello->player_active);
}
static void on_stream_start(void *ctx, const eaf_sendspin_stream_start_t *start) {
    (void)ctx;
    eaf_format_t format = {.sample_rate = start->sample_rate,
                           .num_channels = 2,
                           .channel_mask = EAF_CH_FRONT_LEFT | EAF_CH_FRONT_RIGHT};
    uint32_t capacity = eaf_board_output_capacity_frames();
    int rc = eaf_board_output_start(&format, capacity * 3u / 4u, false);
    if (!rc)
        rc = eaf_sendspin_player_begin(&player, &client.filter, start);
    if (rc)
        printk("Stream start failed rc=%d\n", rc);
    else
        printk("Stream: %u Hz %u-bit %u ch\n", start->sample_rate, start->bit_depth,
               start->channels);
}
static void on_audio(void *ctx, const eaf_sendspin_stream_start_t *format, int64_t timestamp_us,
                     const uint8_t *pcm, size_t length) {
    (void)ctx;
    (void)format;
    uint32_t before = player.frames_written;
    (void)eaf_sendspin_player_write(&player, timestamp_us, pcm, length);
    if (player.frames_written != before)
        atomic_inc(&chunks);
}
static void on_stream_end(void *ctx, bool end_player) {
    (void)ctx;
    if (end_player) {
        eaf_sendspin_player_finish(&player);
        eaf_board_output_finish();
    }
}
static void on_command(void *ctx, const eaf_sendspin_server_command_t *command) {
    (void)ctx;
    (void)command;
}
static void on_disconnect(void *ctx) {
    (void)ctx;
    printk("Sendspin disconnected\n");
}

static void wifi_event(struct net_mgmt_event_callback *cb, uint64_t event,
                       struct net_if *event_iface) {
    if (event_iface != iface)
        return;
    if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        atomic_store(&associated, false);
        printk("Wi-Fi disconnected\n");
    }
    if (event == NET_EVENT_WIFI_CONNECT_RESULT && cb->info &&
        cb->info_length >= sizeof(struct wifi_status)) {
        const struct wifi_status *status = cb->info;
        atomic_store(&associated, status->status == 0);
        printk("Wi-Fi association result: %d\n", status->status);
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
        printk("Wi-Fi timeout\n");
        return EAF_TIMEOUT;
    }
    char address[NET_IPV4_ADDR_LEN];
    struct in_addr *ip = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
    if (!ip)
        return EAF_IO;
    printk("Wi-Fi IPv4: %s\n", net_addr_ntop(AF_INET, ip, address, sizeof(address)));
#if defined(CONFIG_WIFI_ESP32) && defined(CONFIG_EAF_BOARD_WIFI_PS_NONE)
    printk("Wi-Fi power save off rc=%d\n", (int)esp_wifi_set_ps(WIFI_PS_NONE));
#endif
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

    eaf_board_output_config_t output_config = {.audio_cpu = CONFIG_EAF_BOARD_AUDIO_CPU,
                                               .log = board_log};
    if (eaf_board_output_init(&output_config)) {
        printk("Output initialization failed\n");
        return 1;
    }
    eaf_sendspin_player_init(&player, board_write, NULL);

    uint32_t capacity = eaf_board_output_capacity_frames();
    uint32_t capacity_ms = capacity * 1000u / 48000u;
    eaf_sendspin_config_t config = {.client_id = "eaf-sendspin-wroom",
                                    .name = "EAF Sendspin WROOM",
                                    .product_name = "EAF Sendspin WROOM",
                                    .manufacturer = "EAF",
                                    .software_version = "phase2",
                                    .sample_rate = 44100,
                                    .channels = 2,
                                    .bit_depth = 16,
                                    .buffer_capacity = capacity * 2u * 2u,
                                    .support_volume = true,
                                    .support_mute = true,
                                    .volume = 100,
                                    .static_delay_ms = 0,
                                    .required_lead_time_ms = (int32_t)capacity_ms,
                                    .min_buffer_ms = (int32_t)(capacity_ms / 2u)};
    eaf_sendspin_callbacks_t callbacks = {.ready = on_ready,
                                          .stream_start = on_stream_start,
                                          .audio = on_audio,
                                          .stream_end = on_stream_end,
                                          .command = on_command,
                                          .disconnected = on_disconnect};
    eaf_sendspin_client_init(&client, &config, &callbacks, NULL);

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

    printk("EAF Sendspin board: server %s:%d\n", CONFIG_EAF_BOARD_SERVER, CONFIG_EAF_BOARD_PORT);
    for (;;) {
        int rc = online() ? EAF_OK : connect_wifi();
        if (!rc)
            rc = eaf_sendspin_client_connect(&client, sys_be32_to_cpu(server.s_addr),
                                             (uint16_t)CONFIG_EAF_BOARD_PORT, 3000);
        if (rc) {
            printk("Sendspin connect failed rc=%d\n", rc);
            k_sleep(K_SECONDS(3));
            continue;
        }
        printk("Sendspin connected; select this player in Music Assistant\n");
        int64_t report = 0;
        while (!rc && online() && !eaf_board_output_failed()) {
            rc = eaf_sendspin_client_step(&client);
            int64_t now = k_uptime_get();
            if (now >= report) {
                printk("Sendspin chunks=%u written=%u dropped=%u underruns=%u\n",
                       (unsigned)atomic_get(&chunks), player.frames_written, player.frames_dropped,
                       eaf_board_output_underruns());
                report = now + 5000;
            }
            k_sleep(K_MSEC(2));
        }
        eaf_sendspin_client_close(&client);
        eaf_sendspin_player_finish(&player);
        eaf_board_output_stop();
        if (eaf_board_output_failed()) {
            printk("Output failure: playback stopped\n");
            return 1;
        }
        if (!online()) {
            (void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
            net_dhcpv4_stop(iface);
        }
        k_sleep(K_SECONDS(3));
    }
    return 0;
}
