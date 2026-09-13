#pragma once
#include <eaf/eaf_types.h>

/* Sendspin client protocol (captured MA 2.10.3 cleartext revision).
   Storage is caller-owned; no dynamic allocation and no OS headers. */

/* --- Time filter -----------------------------------------------------------
   Two-dimensional Kalman filter tracking server/client offset and clock drift.
   Port of the reference aiosendspin implementation. All timestamps are
   microseconds. See docs/bench/sendspin-capture-2026-09-13.md. */

typedef struct {
    uint64_t last_update;
    double offset;
    double drift;
    bool use_drift;
} eaf_sendspin_time_element_t;

typedef struct {
    uint32_t count;
    uint64_t last_update;
    double offset, drift;
    double offset_covariance, offset_drift_covariance, drift_covariance;
    double process_variance, drift_process_variance, forget_variance_factor;
    eaf_sendspin_time_element_t element;
} eaf_sendspin_time_filter_t;

void eaf_sendspin_time_filter_init(eaf_sendspin_time_filter_t *f);
void eaf_sendspin_time_filter_reset(eaf_sendspin_time_filter_t *f);
/* measurement = ((server_received - client_transmitted) +
                  (server_transmitted - client_received)) / 2
   max_error   = ((client_received - client_transmitted) -
                  (server_transmitted - server_received)) / 2
   Computes the NTP-style offset/uncertainty and feeds the filter. */
void eaf_sendspin_time_filter_exchange(eaf_sendspin_time_filter_t *f, int64_t client_transmitted,
                                       int64_t server_received, int64_t server_transmitted,
                                       uint64_t client_received);
/* Raw update; time_added must be monotonic or the sample is ignored. */
void eaf_sendspin_time_filter_update(eaf_sendspin_time_filter_t *f, int64_t measurement,
                                     int64_t max_error, uint64_t time_added);
int64_t eaf_sendspin_compute_server_time(const eaf_sendspin_time_filter_t *f, int64_t client_time);
int64_t eaf_sendspin_compute_client_time(const eaf_sendspin_time_filter_t *f, int64_t server_time);
bool eaf_sendspin_time_synchronized(const eaf_sendspin_time_filter_t *f);
int64_t eaf_sendspin_time_error(const eaf_sendspin_time_filter_t *f);

/* --- JSON reader -----------------------------------------------------------
   Bounded, non-allocating reader for the flat messages the server sends.
   String views point into the caller's buffer and are not NUL-terminated. */

typedef enum {
    EAF_SENDPIN_JSON_NULL,
    EAF_SENDPIN_JSON_BOOL,
    EAF_SENDPIN_JSON_NUMBER,
    EAF_SENDPIN_JSON_STRING,
    EAF_SENDPIN_JSON_ARRAY,
    EAF_SENDPIN_JSON_OBJECT
} eaf_sendspin_json_type_t;

typedef struct {
    eaf_sendspin_json_type_t type;
    bool boolean;
    int64_t number;
    const char *string;
    size_t string_length;
    const char *raw;
    size_t raw_length;
} eaf_sendspin_json_value_t;

/* Look up a dotted object path, e.g. "payload.player.codec" or "type". */
int eaf_sendspin_json_get(const char *json, size_t length, const char *path,
                          eaf_sendspin_json_value_t *out);
int eaf_sendspin_json_array_contains(const eaf_sendspin_json_value_t *array, const char *needle);
int eaf_sendspin_json_string_equals(const eaf_sendspin_json_value_t *value, const char *text);

/* --- Protocol messages (captured MA 2.10.3 revision) ----------------------- */

typedef enum {
    EAF_SENDPIN_CODEC_UNKNOWN = 0,
    EAF_SENDPIN_CODEC_PCM,
    EAF_SENDPIN_CODEC_FLAC
} eaf_sendspin_codec_t;

typedef enum {
    EAF_SENDPIN_COMMAND_UNKNOWN = 0,
    EAF_SENDPIN_COMMAND_VOLUME,
    EAF_SENDPIN_COMMAND_MUTE,
    EAF_SENDPIN_COMMAND_SET_STATIC_DELAY
} eaf_sendspin_command_t;

typedef struct {
    uint32_t version;
    bool player_active;
    const char *server_id;
    size_t server_id_length;
    const char *name;
    size_t name_length;
    const char *connection_reason;
    size_t connection_reason_length;
} eaf_sendspin_server_hello_t;

typedef struct {
    int64_t client_transmitted, server_received, server_transmitted;
} eaf_sendspin_server_time_t;

typedef struct {
    int64_t server_transmitted;
    eaf_sendspin_codec_t codec;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bit_depth;
    const char *codec_header;
    size_t codec_header_length;
} eaf_sendspin_stream_start_t;

typedef struct {
    int64_t server_transmitted;
    bool end_player;
} eaf_sendspin_stream_end_t;

typedef struct {
    const char *playback_state;
    size_t playback_state_length;
    const char *group_id;
    size_t group_id_length;
} eaf_sendspin_group_update_t;

typedef struct {
    eaf_sendspin_command_t command;
    int32_t volume;
    bool muted;
    int32_t static_delay_ms;
} eaf_sendspin_server_command_t;

int eaf_sendspin_parse_server_hello(const char *json, size_t length,
                                    eaf_sendspin_server_hello_t *out);
int eaf_sendspin_parse_server_time(const char *json, size_t length,
                                   eaf_sendspin_server_time_t *out);
int eaf_sendspin_parse_stream_start(const char *json, size_t length,
                                    eaf_sendspin_stream_start_t *out);
int eaf_sendspin_parse_stream_end(const char *json, size_t length, eaf_sendspin_stream_end_t *out);
int eaf_sendspin_parse_group_update(const char *json, size_t length,
                                    eaf_sendspin_group_update_t *out);
int eaf_sendspin_parse_server_command(const char *json, size_t length,
                                      eaf_sendspin_server_command_t *out);

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
} eaf_sendspin_client_hello_t;

typedef struct {
    int32_t volume;
    bool muted;
    int32_t static_delay_ms;
    int32_t required_lead_time_ms;
    int32_t min_buffer_ms;
} eaf_sendspin_client_state_t;

int eaf_sendspin_build_client_hello(char *dst, size_t capacity,
                                    const eaf_sendspin_client_hello_t *hello, size_t *written);
int eaf_sendspin_build_client_time(char *dst, size_t capacity, int64_t client_transmitted,
                                   size_t *written);
int eaf_sendspin_build_client_state(char *dst, size_t capacity,
                                    const eaf_sendspin_client_state_t *state, size_t *written);
int eaf_sendspin_build_client_goodbye(char *dst, size_t capacity, const char *reason,
                                      size_t *written);
