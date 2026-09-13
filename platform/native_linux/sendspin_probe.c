#include <eaf/eaf_hal.h>
#include <eaf/eaf_sendspin_client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static eaf_sendspin_client_t client;
static unsigned audio_chunks;
static unsigned stream_starts, stream_ends;
static int disconnected;

static void on_ready(void *ctx, const eaf_sendspin_server_hello_t *hello) {
    (void)ctx;
    printf("READY server=\"%.*s\" id=\"%.*s\" version=%u player_active=%d\n",
           (int)hello->name_length, hello->name, (int)hello->server_id_length, hello->server_id,
           hello->version, (int)hello->player_active);
}
static void on_stream_start(void *ctx, const eaf_sendspin_stream_start_t *start) {
    (void)ctx;
    ++stream_starts;
    printf("STREAM start codec=%d rate=%u channels=%u depth=%u lead_us=%lld\n", (int)start->codec,
           start->sample_rate, start->channels, start->bit_depth,
           (long long)start->server_transmitted);
}
static void on_audio(void *ctx, const eaf_sendspin_stream_start_t *format, int64_t timestamp_us,
                     const uint8_t *pcm, size_t length) {
    (void)ctx;
    (void)format;
    if (audio_chunks % 50u == 0u)
        printf("AUDIO chunk=%u ts_us=%lld bytes=%zu first16=%02x%02x%02x%02x\n", audio_chunks,
               (long long)timestamp_us, length, pcm[0], pcm[1], pcm[2], pcm[3]);
    ++audio_chunks;
}
static void on_stream_end(void *ctx, bool player) {
    (void)ctx;
    ++stream_ends;
    printf("STREAM end player=%d\n", (int)player);
}
static void on_command(void *ctx, const eaf_sendspin_server_command_t *command) {
    (void)ctx;
    printf("COMMAND cmd=%d volume=%d muted=%d delay=%d\n", (int)command->command, command->volume,
           (int)command->muted, command->static_delay_ms);
}
static void on_disconnect(void *ctx) {
    (void)ctx;
    disconnected = 1;
}

static int parse_ipv4(const char *text, uint32_t *out) {
    uint32_t parts[4];
    const char *p = text;
    for (size_t i = 0; i < 4; ++i) {
        char *end = NULL;
        unsigned long value = strtoul(p, &end, 10);
        if (end == p || value > 255u)
            return -1;
        parts[i] = (uint32_t)value;
        p = end;
        if (i < 3) {
            if (*p != '.')
                return -1;
            ++p;
        } else if (*p != '\0') {
            return -1;
        }
    }
    *out = parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3];
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <ipv4> [port] [seconds]\n", argv[0]);
        return 2;
    }
    uint32_t ip = 0;
    if (parse_ipv4(argv[1], &ip)) {
        fprintf(stderr, "invalid IPv4: %s\n", argv[1]);
        return 2;
    }
    unsigned port = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 8927u;
    unsigned seconds = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 20u;

    eaf_sendspin_config_t config = {.client_id = "eaf-native-probe",
                                    .name = "EAF Native Probe",
                                    .product_name = "EAF Native Probe",
                                    .manufacturer = "EAF",
                                    .software_version = "phase1",
                                    .sample_rate = 44100,
                                    .channels = 2,
                                    .bit_depth = 16,
                                    .buffer_capacity = 262144,
                                    .support_volume = true,
                                    .support_mute = true,
                                    .volume = 100,
                                    .static_delay_ms = 0,
                                    .required_lead_time_ms = 250,
                                    .min_buffer_ms = 250};
    eaf_sendspin_callbacks_t callbacks = {.ready = on_ready,
                                          .stream_start = on_stream_start,
                                          .audio = on_audio,
                                          .stream_end = on_stream_end,
                                          .command = on_command,
                                          .disconnected = on_disconnect};
    eaf_sendspin_client_init(&client, &config, &callbacks, NULL);
    int rc = eaf_sendspin_client_connect(&client, ip, (uint16_t)port, 3000);
    if (rc) {
        fprintf(stderr, "connect failed: %d\n", rc);
        return 1;
    }

    uint64_t deadline = hal_monotonic_time_us() + (uint64_t)seconds * 1000000u;
    bool reported_sync = false;
    while (hal_monotonic_time_us() < deadline && !disconnected) {
        if (eaf_sendspin_client_step(&client) == EAF_IO)
            break;
        if (!reported_sync && eaf_sendspin_client_ready(&client) &&
            eaf_sendspin_client_time_synchronized(&client)) {
            reported_sync = true;
            printf("SYNC converged after handshake\n");
        }
        hal_sleep_ms(5);
    }
    printf("SUMMARY ready=%d time_synced=%d stream_starts=%u stream_ends=%u audio_chunks=%u "
           "disconnected=%d\n",
           (int)eaf_sendspin_client_ready(&client),
           (int)eaf_sendspin_client_time_synchronized(&client), stream_starts, stream_ends,
           audio_chunks, disconnected);
    bool ready = eaf_sendspin_client_ready(&client);
    eaf_sendspin_client_close(&client);
    return ready ? 0 : 1;
}
