#include "board_config.h"
#include "board_runtime.h"
#include "diagnostics.h"
#include "ota.h"
#include "tas5805m.h"
#include <board_output.h>
#include <eaf/eaf_hal.h>
#include <eaf/eaf_sendspin_client.h>
#include <eaf/eaf_sendspin_player.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/sys/reboot.h>

/* dhcpv4.h requires the net_if declaration first. */
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

static void sendspin_output_log(const char *message) {
    board_log("%s", message);
}

static uint32_t board_write(void *ctx, const int32_t *samples, uint32_t frames) {
    (void)ctx;
    return eaf_board_output_write(samples, frames);
}
static void on_ready(void *ctx, const eaf_sendspin_server_hello_t *hello) {
    (void)ctx;
    board_log("Sendspin ready: %.*s player=%d\n", (int)hello->name_length, hello->name,
              (int)hello->player_active);
}
static eaf_sendspin_stream_start_t current_stream;
static bool have_stream;

static void apply_volume(void);

static void start_output(const eaf_sendspin_stream_start_t *start) {
    have_stream = false;
    eaf_sendspin_player_finish(&player);
    eaf_format_t format = {.sample_rate = start->sample_rate,
                           .num_channels = 2,
                           .channel_mask = EAF_CH_FRONT_LEFT | EAF_CH_FRONT_RIGHT};
    uint32_t capacity = eaf_board_output_capacity_frames();
    int rc = eaf_board_output_replace(&format, capacity * 3u / 4u, false);
    if (!rc)
        rc = eaf_sendspin_player_begin(&player, &client.filter, start);
    if (rc) {
        board_log("Stream start failed rc=%d\n", rc);
    } else {
        have_stream = true;
        apply_volume();
        (void)tas5805m_play();
        board_log("Stream: %u Hz %u-bit %u ch\n", start->sample_rate, start->bit_depth,
                  start->channels);
    }
}
static void on_stream_start(void *ctx, const eaf_sendspin_stream_start_t *start) {
    (void)ctx;
    current_stream = *start;
    start_output(start);
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
        have_stream = false;
        eaf_sendspin_player_finish(&player);
        eaf_board_output_finish();
    }
}
static void on_stream_clear(void *ctx, bool clear_player) {
    (void)ctx;
    if (clear_player && have_stream) {
        board_log("Stream clear; flushing buffered audio\n");
        start_output(&current_stream);
    }
}
static int32_t volume_level = 100;
static bool volume_muted;

/* Perceptual square-law mapping from the server's 0..100 to Q1.31 gain. */
static int32_t volume_to_gain(int32_t percent) {
    if (percent <= 0)
        return 0;
    if (percent >= 100)
        return INT32_MAX;
    int64_t value = percent;
    return (int32_t)(value * value * INT32_MAX / 10000);
}
static void apply_volume(void) {
    int32_t gain = volume_muted ? 0 : volume_to_gain(volume_level);
    (void)eaf_board_output_volume(gain, gain);
}
static void on_command(void *ctx, const eaf_sendspin_server_command_t *command) {
    (void)ctx;
    if (command->command == EAF_SENDPIN_COMMAND_VOLUME && command->volume >= 0) {
        volume_level = command->volume;
        board_log("Volume=%d\n", volume_level);
        apply_volume();
    } else if (command->command == EAF_SENDPIN_COMMAND_MUTE) {
        volume_muted = command->muted;
        board_log("Mute=%d\n", (int)volume_muted);
        apply_volume();
    }
}
static void on_disconnect(void *ctx) {
    (void)ctx;
    board_log("Sendspin disconnected\n");
}

static void wifi_event(struct net_mgmt_event_callback *cb, uint64_t event,
                       struct net_if *event_iface) {
    if (event_iface != iface)
        return;
    if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        atomic_store(&associated, false);
        board_log("Wi-Fi disconnected\n");
    }
    if (event == NET_EVENT_WIFI_CONNECT_RESULT && cb->info &&
        cb->info_length >= sizeof(struct wifi_status)) {
        const struct wifi_status *status = cb->info;
        atomic_store(&associated, status->status == 0);
        board_log("Wi-Fi association result: %d\n", status->status);
    }
}
static bool online(void) {
    return atomic_load(&associated) &&
           net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL;
}
static void wifi_health(void) {
    struct wifi_iface_status status;
    if (!net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status, sizeof(status)))
        board_log("Wi-Fi rssi=%d dtim=%u\n", status.rssi, (unsigned)status.dtim_period);
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
        board_log("Wi-Fi timeout\n");
        return EAF_TIMEOUT;
    }
    char address[NET_IPV4_ADDR_LEN];
    struct in_addr *ip = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
    if (!ip)
        return EAF_IO;
    board_log("Wi-Fi IPv4: %s\n", net_addr_ntop(AF_INET, ip, address, sizeof(address)));
    board_wifi_power_save_off();
    return EAF_OK;
}

int main(void) {
    board_diagnostics_start();
    const char *ssid = board_wifi_ssid(), *password = board_wifi_password();
    size_t ssid_length = strlen(ssid), password_length = strlen(password);
    if (!ssid_length || ssid_length > 32 || password_length < 8 || password_length > 63) {
        board_log("Set a 2.4 GHz WPA2 SSID/passphrase in credentials.local.h and rebuild\n");
        return 1;
    }
    iface = net_if_get_first_wifi();
    if (!iface) {
        board_log("No Wi-Fi interface\n");
        return 1;
    }
    struct in_addr server;
    if (!strlen(CONFIG_EAF_BOARD_SERVER) ||
        net_addr_pton(AF_INET, CONFIG_EAF_BOARD_SERVER, &server)) {
        board_log("Set CONFIG_EAF_BOARD_SERVER to the Music Assistant IPv4 address\n");
        return 1;
    }

    int32_t *storage;
    uint32_t frames;
    if (eaf_board_output_storage(&storage, &frames)) {
        board_log("Output storage allocation failed\n");
        return 1;
    }
    eaf_board_output_config_t output_config = {.audio_cpu = CONFIG_EAF_BOARD_AUDIO_CPU,
                                               .log = sendspin_output_log,
                                               .storage = storage,
                                               .capacity_frames = frames};
    if (eaf_board_output_init(&output_config)) {
        board_log("Output initialization failed\n");
        return 1;
    }
    (void)tas5805m_bringup();
    eaf_sendspin_player_init(&player, board_write, NULL);

    uint32_t capacity = eaf_board_output_capacity_frames();
    uint32_t capacity_ms = capacity * 1000u / CONFIG_EAF_SAMPLE_RATE;
    eaf_sendspin_config_t config = {.client_id = CONFIG_EAF_BOARD_CLIENT_ID,
                                    .name = CONFIG_EAF_BOARD_NAME,
                                    .product_name = CONFIG_EAF_BOARD_NAME,
                                    .manufacturer = "EAF",
                                    .software_version = "phase2",
                                    .sample_rate = CONFIG_EAF_SAMPLE_RATE,
                                    .bit_depth = 16,
                                    .support_volume = true,
                                    .support_mute = true,
                                    .volume = 100,
                                    .static_delay_ms = 0,
                                    .required_lead_time_ms = (int32_t)capacity_ms,
                                    .min_buffer_ms = (int32_t)capacity_ms};
    config.channels = CONFIG_EAF_SOURCE_CHANNELS;
    config.buffer_capacity = capacity * CONFIG_EAF_BYTES_PER_FRAME;
    eaf_sendspin_callbacks_t callbacks = {.ready = on_ready,
                                          .stream_start = on_stream_start,
                                          .audio = on_audio,
                                          .stream_end = on_stream_end,
                                          .stream_clear = on_stream_clear,
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

    board_log("EAF Sendspin board: server %s:%d\n", CONFIG_EAF_BOARD_SERVER, CONFIG_EAF_BOARD_PORT);
    for (;;) {
        int rc = online() ? EAF_OK : connect_wifi();
        if (!rc)
            rc = eaf_sendspin_client_connect(&client, sys_be32_to_cpu(server.s_addr),
                                             (uint16_t)CONFIG_EAF_BOARD_PORT, 3000);
        if (rc) {
            board_log("Sendspin connect failed rc=%d\n", rc);
            k_sleep(K_SECONDS(3));
            continue;
        }
        board_log("Sendspin connected; select this player in Music Assistant\n");
        int64_t report = 0;
        uint64_t previous_rx = client.rx_total;
        int64_t previous_ms = k_uptime_get();
        while (!rc && online() && !eaf_board_output_failed()) {
            /* Drain the socket greedily so the server's TCP window stays open. */
            uint64_t before = client.rx_total;
            for (unsigned i = 0; i < 8 && !rc; ++i) {
                rc = eaf_sendspin_client_step(&client);
                if (client.rx_total == before)
                    break;
                before = client.rx_total;
            }
            int64_t now = k_uptime_get();
            if (now >= report) {
                uint64_t span = (uint64_t)(now - previous_ms);
                uint64_t rate = span ? (client.rx_total - previous_rx) * 1000u / span : 0;
                board_log("Sendspin chunks=%u written=%u dropped=%u underruns=%u sync=%d "
                          "lat_ms=%lld level=%u flags=%u rx=%llu B/s vol=%d mute=%d fault=%d\n",
                          (unsigned)atomic_get(&chunks), player.frames_written,
                          player.frames_dropped, eaf_board_output_underruns(),
                          (int)player.synchronized, (long long)(player.last_latency_us / 1000),
                          eaf_board_output_level(), eaf_board_output_flags(),
                          (unsigned long long)rate, volume_level, (int)volume_muted,
                          (int)tas5805m_fault());
                wifi_health();
                previous_rx = client.rx_total;
                previous_ms = now;
                report = now + 5000;
            }
            if (ota_reboot_requested()) {
                board_log("OTA reboot\n");
                k_sleep(K_MSEC(200));
                sys_reboot(SYS_REBOOT_COLD);
            }
            k_sleep(K_MSEC(1));
        }
        eaf_sendspin_client_close(&client);
        eaf_sendspin_player_finish(&player);
        eaf_board_output_stop();
        if (eaf_board_output_failed()) {
            board_log("Output failure: playback stopped\n");
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
