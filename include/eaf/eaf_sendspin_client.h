#pragma once
#include <eaf/eaf_net.h>
#include <eaf/eaf_sendspin.h>

#ifndef EAF_SENDPIN_RX_BYTES
#define EAF_SENDPIN_RX_BYTES 4096u
#endif
#ifndef EAF_SENDPIN_MESSAGE_BYTES
#define EAF_SENDPIN_MESSAGE_BYTES 8192u
#endif
#ifndef EAF_SENDPIN_JSON_BYTES
#define EAF_SENDPIN_JSON_BYTES 1024u
#endif
#ifndef EAF_SENDPIN_WIRE_BYTES
#define EAF_SENDPIN_WIRE_BYTES 2048u
#endif
#ifndef EAF_SENDPIN_HTTP_BYTES
#define EAF_SENDPIN_HTTP_BYTES 1024u
#endif

typedef struct {
    const char *client_id;
    const char *name;
    const char *product_name;
    const char *manufacturer;
    const char *software_version;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bit_depth;
    uint32_t buffer_capacity;
    bool support_volume;
    bool support_mute;
    int32_t volume;
    int32_t static_delay_ms;
    int32_t required_lead_time_ms;
    int32_t min_buffer_ms;
} eaf_sendspin_config_t;

typedef struct {
    void (*ready)(void *ctx, const eaf_sendspin_server_hello_t *hello);
    void (*stream_start)(void *ctx, const eaf_sendspin_stream_start_t *start);
    void (*audio)(void *ctx, const eaf_sendspin_stream_start_t *format, int64_t timestamp_us,
                  const uint8_t *pcm, size_t length);
    void (*stream_end)(void *ctx, bool player);
    void (*group_update)(void *ctx, const eaf_sendspin_group_update_t *group);
    void (*command)(void *ctx, const eaf_sendspin_server_command_t *command);
    void (*disconnected)(void *ctx);
} eaf_sendspin_callbacks_t;

typedef enum {
    EAF_SENDPIN_CLIENT_CLOSED = 0,
    EAF_SENDPIN_CLIENT_HTTP,
    EAF_SENDPIN_CLIENT_WS
} eaf_sendspin_client_state_t;

typedef struct {
    eaf_tcp_t tcp;
    eaf_sendspin_config_t config;
    eaf_sendspin_callbacks_t callbacks;
    void *callback_ctx;
    eaf_sendspin_time_filter_t filter;
    eaf_sendspin_ws_rx_t rx;
    eaf_sendspin_stream_start_t stream;
    uint8_t rx_bytes[EAF_SENDPIN_RX_BYTES];
    uint8_t message[EAF_SENDPIN_MESSAGE_BYTES];
    uint8_t frame[EAF_SENDPIN_JSON_BYTES + EAF_SENDPIN_WS_HEADER_MAX];
    uint8_t wire[EAF_SENDPIN_WIRE_BYTES];
    char json[EAF_SENDPIN_JSON_BYTES];
    char http[EAF_SENDPIN_HTTP_BYTES];
    size_t http_used;
    size_t wire_length, wire_offset;
    eaf_sendspin_ws_prng_t prng;
    eaf_sendspin_client_state_t state;
    bool handshaken, notified, stream_active;
    uint64_t next_time_us;
} eaf_sendspin_client_t;

void eaf_sendspin_client_init(eaf_sendspin_client_t *client, const eaf_sendspin_config_t *config,
                              const eaf_sendspin_callbacks_t *callbacks, void *ctx);
/* Blocking TCP connect, then starts the WebSocket Upgrade. */
int eaf_sendspin_client_connect(eaf_sendspin_client_t *client, uint32_t ipv4, uint16_t port,
                                uint32_t timeout_ms);
/* One non-blocking iteration: flush, advance the handshake, receive and dispatch. */
int eaf_sendspin_client_step(eaf_sendspin_client_t *client);
void eaf_sendspin_client_close(eaf_sendspin_client_t *client);

bool eaf_sendspin_client_ready(const eaf_sendspin_client_t *client);
bool eaf_sendspin_client_time_synchronized(const eaf_sendspin_client_t *client);
int64_t eaf_sendspin_client_compute_client_time(const eaf_sendspin_client_t *client,
                                                int64_t server_us);
int64_t eaf_sendspin_client_compute_server_time(const eaf_sendspin_client_t *client,
                                                int64_t client_us);
