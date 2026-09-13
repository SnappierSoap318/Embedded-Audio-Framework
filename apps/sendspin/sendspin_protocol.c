#include <eaf/eaf_sendspin.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    const char *end;
} cursor_t;

static void skip_ws(cursor_t *c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
        ++c->p;
}

static int scan_string(cursor_t *c, const char **start, size_t *length) {
    if (c->p >= c->end || *c->p != '"')
        return EAF_INVALID;
    ++c->p;
    const char *s = c->p;
    while (c->p < c->end) {
        if (*c->p == '\\') {
            if (c->end - c->p < 2)
                return EAF_INVALID;
            c->p += 2;
            continue;
        }
        if (*c->p == '"') {
            *start = s;
            *length = (size_t)(c->p - s);
            ++c->p;
            return EAF_OK;
        }
        ++c->p;
    }
    return EAF_INVALID;
}

static int match_literal(cursor_t *c, const char *literal) {
    size_t n = strlen(literal);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, literal, n) != 0)
        return EAF_INVALID;
    c->p += n;
    return EAF_OK;
}

static int skip_number(cursor_t *c) {
    if (c->p < c->end && *c->p == '-')
        ++c->p;
    if (c->p >= c->end || *c->p < '0' || *c->p > '9')
        return EAF_INVALID;
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9')
        ++c->p;
    if (c->p < c->end && *c->p == '.') {
        ++c->p;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9')
            ++c->p;
    }
    if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
        ++c->p;
        if (c->p < c->end && (*c->p == '+' || *c->p == '-'))
            ++c->p;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9')
            ++c->p;
    }
    return EAF_OK;
}

static int skip_value(cursor_t *c);

static int skip_object(cursor_t *c) {
    ++c->p;
    skip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        ++c->p;
        return EAF_OK;
    }
    for (;;) {
        const char *key;
        size_t key_length;
        int rc = scan_string(c, &key, &key_length);
        if (rc)
            return rc;
        (void)key;
        (void)key_length;
        skip_ws(c);
        if (c->p >= c->end || *c->p != ':')
            return EAF_INVALID;
        ++c->p;
        rc = skip_value(c);
        if (rc)
            return rc;
        skip_ws(c);
        if (c->p >= c->end)
            return EAF_INVALID;
        if (*c->p == ',') {
            ++c->p;
            continue;
        }
        if (*c->p == '}') {
            ++c->p;
            return EAF_OK;
        }
        return EAF_INVALID;
    }
}

static int skip_array(cursor_t *c) {
    ++c->p;
    skip_ws(c);
    if (c->p < c->end && *c->p == ']') {
        ++c->p;
        return EAF_OK;
    }
    for (;;) {
        int rc = skip_value(c);
        if (rc)
            return rc;
        skip_ws(c);
        if (c->p >= c->end)
            return EAF_INVALID;
        if (*c->p == ',') {
            ++c->p;
            continue;
        }
        if (*c->p == ']') {
            ++c->p;
            return EAF_OK;
        }
        return EAF_INVALID;
    }
}

static int skip_value(cursor_t *c) {
    skip_ws(c);
    if (c->p >= c->end)
        return EAF_INVALID;
    const char *s;
    size_t n;
    switch (*c->p) {
    case '{':
        return skip_object(c);
    case '[':
        return skip_array(c);
    case '"':
        return scan_string(c, &s, &n);
    case 't':
        return match_literal(c, "true");
    case 'f':
        return match_literal(c, "false");
    case 'n':
        return match_literal(c, "null");
    default:
        return skip_number(c);
    }
}

static int parse_number(cursor_t *c, int64_t *out) {
    const char *start = c->p;
    int rc = skip_number(c);
    if (rc)
        return rc;
    char buffer[32];
    size_t n = (size_t)(c->p - start);
    if (n >= sizeof(buffer))
        return EAF_INVALID;
    memcpy(buffer, start, n);
    buffer[n] = '\0';
    char *end = NULL;
    long long value = strtoll(buffer, &end, 10);
    if (!end || *end != '\0')
        return EAF_INVALID;
    *out = (int64_t)value;
    return EAF_OK;
}

static int parse_value(cursor_t *c, eaf_sendspin_json_value_t *out) {
    memset(out, 0, sizeof(*out));
    skip_ws(c);
    if (c->p >= c->end)
        return EAF_INVALID;
    const char *start = c->p;
    switch (*c->p) {
    case '{':
        out->type = EAF_SENDPIN_JSON_OBJECT;
        if (skip_object(c))
            return EAF_INVALID;
        break;
    case '[':
        out->type = EAF_SENDPIN_JSON_ARRAY;
        if (skip_array(c))
            return EAF_INVALID;
        break;
    case '"': {
        out->type = EAF_SENDPIN_JSON_STRING;
        if (scan_string(c, &out->string, &out->string_length))
            return EAF_INVALID;
        break;
    }
    case 't':
        out->type = EAF_SENDPIN_JSON_BOOL;
        out->boolean = true;
        if (match_literal(c, "true"))
            return EAF_INVALID;
        break;
    case 'f':
        out->type = EAF_SENDPIN_JSON_BOOL;
        out->boolean = false;
        if (match_literal(c, "false"))
            return EAF_INVALID;
        break;
    case 'n':
        out->type = EAF_SENDPIN_JSON_NULL;
        if (match_literal(c, "null"))
            return EAF_INVALID;
        break;
    default:
        out->type = EAF_SENDPIN_JSON_NUMBER;
        if (parse_number(c, &out->number))
            return EAF_INVALID;
        break;
    }
    out->raw = start;
    out->raw_length = (size_t)(c->p - start);
    return EAF_OK;
}

static int descend(cursor_t *c, const char *segment, size_t segment_length,
                   eaf_sendspin_json_value_t *out, bool leaf) {
    skip_ws(c);
    if (c->p >= c->end || *c->p != '{')
        return EAF_INVALID;
    ++c->p;
    for (;;) {
        skip_ws(c);
        if (c->p >= c->end)
            return EAF_INVALID;
        if (*c->p == '}') {
            ++c->p;
            return EAF_UNSUPPORTED;
        }
        const char *key;
        size_t key_length;
        int rc = scan_string(c, &key, &key_length);
        if (rc)
            return rc;
        skip_ws(c);
        if (c->p >= c->end || *c->p != ':')
            return EAF_INVALID;
        ++c->p;
        if (key_length == segment_length && !memcmp(key, segment, segment_length))
            return leaf ? parse_value(c, out) : EAF_OK;
        rc = skip_value(c);
        if (rc)
            return rc;
        skip_ws(c);
        if (c->p >= c->end)
            return EAF_INVALID;
        if (*c->p == ',') {
            ++c->p;
            continue;
        }
        if (*c->p == '}') {
            ++c->p;
            return EAF_UNSUPPORTED;
        }
        return EAF_INVALID;
    }
}

int eaf_sendspin_json_get(const char *json, size_t length, const char *path,
                          eaf_sendspin_json_value_t *out) {
    if (!json || !path || !out || !*path)
        return EAF_INVALID;
    cursor_t c = {json, json + length};
    const char *segment = path;
    for (;;) {
        const char *dot = strchr(segment, '.');
        size_t segment_length = dot ? (size_t)(dot - segment) : strlen(segment);
        bool leaf = dot == NULL;
        int rc = descend(&c, segment, segment_length, out, leaf);
        if (rc)
            return rc;
        if (leaf)
            return EAF_OK;
        segment = dot + 1;
    }
}

int eaf_sendspin_json_array_contains(const eaf_sendspin_json_value_t *array, const char *needle) {
    if (!array || array->type != EAF_SENDPIN_JSON_ARRAY || !needle)
        return 0;
    size_t needle_length = strlen(needle);
    cursor_t c = {array->raw, array->raw + array->raw_length};
    ++c.p;
    skip_ws(&c);
    if (c.p < c.end && *c.p == ']')
        return 0;
    for (;;) {
        skip_ws(&c);
        const char *element;
        size_t element_length;
        if (c.p < c.end && *c.p == '"' && !scan_string(&c, &element, &element_length)) {
            if (element_length == needle_length && !memcmp(element, needle, needle_length))
                return 1;
        } else if (skip_value(&c)) {
            return 0;
        }
        skip_ws(&c);
        if (c.p >= c.end)
            return 0;
        if (*c.p == ',') {
            ++c.p;
            continue;
        }
        return 0;
    }
}

int eaf_sendspin_json_string_equals(const eaf_sendspin_json_value_t *value, const char *text) {
    size_t n = strlen(text);
    return value && value->type == EAF_SENDPIN_JSON_STRING && value->string_length == n &&
           !memcmp(value->string, text, n);
}

static int get_number(const char *json, size_t length, const char *path, int64_t *out) {
    eaf_sendspin_json_value_t value;
    int rc = eaf_sendspin_json_get(json, length, path, &value);
    if (rc || value.type != EAF_SENDPIN_JSON_NUMBER)
        return EAF_INVALID;
    *out = value.number;
    return EAF_OK;
}

static eaf_sendspin_codec_t codec_from_value(const eaf_sendspin_json_value_t *value) {
    if (eaf_sendspin_json_string_equals(value, "pcm"))
        return EAF_SENDPIN_CODEC_PCM;
    if (eaf_sendspin_json_string_equals(value, "flac"))
        return EAF_SENDPIN_CODEC_FLAC;
    return EAF_SENDPIN_CODEC_UNKNOWN;
}

int eaf_sendspin_parse_server_hello(const char *json, size_t length,
                                    eaf_sendspin_server_hello_t *out) {
    if (!json || !out)
        return EAF_INVALID;
    *out = (eaf_sendspin_server_hello_t){0};
    eaf_sendspin_json_value_t value;
    if (eaf_sendspin_json_get(json, length, "payload.active_roles", &value))
        return EAF_INVALID;
    out->player_active = eaf_sendspin_json_array_contains(&value, "player@v1") != 0;
    if (!eaf_sendspin_json_get(json, length, "payload.server_id", &value) &&
        value.type == EAF_SENDPIN_JSON_STRING) {
        out->server_id = value.string;
        out->server_id_length = value.string_length;
    }
    if (!eaf_sendspin_json_get(json, length, "payload.name", &value) &&
        value.type == EAF_SENDPIN_JSON_STRING) {
        out->name = value.string;
        out->name_length = value.string_length;
    }
    if (!eaf_sendspin_json_get(json, length, "payload.connection_reason", &value) &&
        value.type == EAF_SENDPIN_JSON_STRING) {
        out->connection_reason = value.string;
        out->connection_reason_length = value.string_length;
    }
    int64_t version = 0;
    if (!get_number(json, length, "payload.version", &version))
        out->version = (uint32_t)version;
    return EAF_OK;
}

int eaf_sendspin_parse_server_time(const char *json, size_t length,
                                   eaf_sendspin_server_time_t *out) {
    if (!json || !out)
        return EAF_INVALID;
    return get_number(json, length, "payload.client_transmitted", &out->client_transmitted) ||
                   get_number(json, length, "payload.server_received", &out->server_received) ||
                   get_number(json, length, "payload.server_transmitted", &out->server_transmitted)
               ? EAF_INVALID
               : EAF_OK;
}

int eaf_sendspin_parse_stream_start(const char *json, size_t length,
                                    eaf_sendspin_stream_start_t *out) {
    if (!json || !out)
        return EAF_INVALID;
    *out = (eaf_sendspin_stream_start_t){0};
    eaf_sendspin_json_value_t value;
    int64_t number = 0;
    if (eaf_sendspin_json_get(json, length, "payload.player.codec", &value))
        return EAF_INVALID;
    out->codec = codec_from_value(&value);
    if (get_number(json, length, "payload.player.sample_rate", &number))
        return EAF_INVALID;
    out->sample_rate = (uint32_t)number;
    if (get_number(json, length, "payload.player.channels", &number))
        return EAF_INVALID;
    out->channels = (uint8_t)number;
    if (get_number(json, length, "payload.player.bit_depth", &number))
        return EAF_INVALID;
    out->bit_depth = (uint8_t)number;
    if (get_number(json, length, "payload.server_transmitted", &out->server_transmitted))
        out->server_transmitted = 0;
    if (!eaf_sendspin_json_get(json, length, "payload.player.codec_header", &value) &&
        value.type == EAF_SENDPIN_JSON_STRING) {
        out->codec_header = value.string;
        out->codec_header_length = value.string_length;
    }
    return EAF_OK;
}

int eaf_sendspin_parse_stream_end(const char *json, size_t length, eaf_sendspin_stream_end_t *out) {
    if (!json || !out)
        return EAF_INVALID;
    *out = (eaf_sendspin_stream_end_t){0};
    if (get_number(json, length, "payload.server_transmitted", &out->server_transmitted))
        out->server_transmitted = 0;
    eaf_sendspin_json_value_t value;
    int rc = eaf_sendspin_json_get(json, length, "payload.roles", &value);
    if (rc == EAF_UNSUPPORTED)
        out->end_player = true;
    else if (rc)
        return EAF_INVALID;
    else
        out->end_player = eaf_sendspin_json_array_contains(&value, "player") != 0;
    return EAF_OK;
}

int eaf_sendspin_parse_group_update(const char *json, size_t length,
                                    eaf_sendspin_group_update_t *out) {
    if (!json || !out)
        return EAF_INVALID;
    *out = (eaf_sendspin_group_update_t){0};
    eaf_sendspin_json_value_t value;
    if (!eaf_sendspin_json_get(json, length, "payload.playback_state", &value) &&
        value.type == EAF_SENDPIN_JSON_STRING) {
        out->playback_state = value.string;
        out->playback_state_length = value.string_length;
    }
    if (!eaf_sendspin_json_get(json, length, "payload.group_id", &value) &&
        value.type == EAF_SENDPIN_JSON_STRING) {
        out->group_id = value.string;
        out->group_id_length = value.string_length;
    }
    return EAF_OK;
}

int eaf_sendspin_parse_server_command(const char *json, size_t length,
                                      eaf_sendspin_server_command_t *out) {
    if (!json || !out)
        return EAF_INVALID;
    *out = (eaf_sendspin_server_command_t){
        .command = EAF_SENDPIN_COMMAND_UNKNOWN, .volume = -1, .static_delay_ms = -1};
    eaf_sendspin_json_value_t value;
    if (eaf_sendspin_json_get(json, length, "payload.player.command", &value))
        return EAF_INVALID;
    if (eaf_sendspin_json_string_equals(&value, "volume"))
        out->command = EAF_SENDPIN_COMMAND_VOLUME;
    else if (eaf_sendspin_json_string_equals(&value, "mute"))
        out->command = EAF_SENDPIN_COMMAND_MUTE;
    else if (eaf_sendspin_json_string_equals(&value, "set_static_delay"))
        out->command = EAF_SENDPIN_COMMAND_SET_STATIC_DELAY;
    if (!eaf_sendspin_json_get(json, length, "payload.player.volume", &value) &&
        value.type == EAF_SENDPIN_JSON_NUMBER)
        out->volume = (int32_t)value.number;
    else if (!eaf_sendspin_json_get(json, length, "payload.player.mute", &value) &&
             value.type == EAF_SENDPIN_JSON_BOOL)
        out->muted = value.boolean;
    else if (!eaf_sendspin_json_get(json, length, "payload.player.static_delay_ms", &value) &&
             value.type == EAF_SENDPIN_JSON_NUMBER)
        out->static_delay_ms = (int32_t)value.number;
    return EAF_OK;
}

/* --- Builders -------------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t capacity;
    size_t used;
    bool ok;
} writer_t;

static void wr_raw(writer_t *w, const char *data, size_t length) {
    if (!w->ok || w->used + length > w->capacity) {
        w->ok = false;
        return;
    }
    memcpy(w->buf + w->used, data, length);
    w->used += length;
}

static void wr_char(writer_t *w, char c) {
    wr_raw(w, &c, 1);
}

static void wr_lit(writer_t *w, const char *literal) {
    wr_raw(w, literal, strlen(literal));
}

static void wr_u64(writer_t *w, uint64_t value) {
    char digits[20];
    size_t n = 0;
    do {
        digits[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value);
    while (n)
        wr_char(w, digits[--n]);
}

static void wr_i64(writer_t *w, int64_t value) {
    if (value < 0) {
        wr_char(w, '-');
        wr_u64(w, (uint64_t)(-(value + 1)) + 1u);
    } else {
        wr_u64(w, (uint64_t)value);
    }
}

static void wr_json_string(writer_t *w, const char *data, size_t length) {
    static const char hex[] = "0123456789abcdef";
    wr_char(w, '"');
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)data[i];
        switch (c) {
        case '"':
            wr_lit(w, "\\\"");
            break;
        case '\\':
            wr_lit(w, "\\\\");
            break;
        case '\b':
            wr_lit(w, "\\b");
            break;
        case '\f':
            wr_lit(w, "\\f");
            break;
        case '\n':
            wr_lit(w, "\\n");
            break;
        case '\r':
            wr_lit(w, "\\r");
            break;
        case '\t':
            wr_lit(w, "\\t");
            break;
        default:
            if (c < 0x20u) {
                char escape[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 0x0fu]};
                wr_raw(w, escape, sizeof(escape));
            } else {
                wr_char(w, (char)c);
            }
            break;
        }
    }
    wr_char(w, '"');
}

static const char *codec_name(eaf_sendspin_codec_t codec) {
    return codec == EAF_SENDPIN_CODEC_FLAC ? "flac" : "pcm";
}

static int finish(writer_t *w, size_t *written) {
    if (!w->ok)
        return EAF_INVALID;
    *written = w->used;
    return EAF_OK;
}

int eaf_sendspin_build_client_hello(char *dst, size_t capacity,
                                    const eaf_sendspin_client_hello_t *hello, size_t *written) {
    if (!dst || !hello || !hello->client_id || !hello->name || !written || !capacity)
        return EAF_INVALID;
    writer_t w = {dst, capacity, 0, true};
    wr_lit(&w, "{\"payload\":{\"client_id\":");
    wr_json_string(&w, hello->client_id, strlen(hello->client_id));
    wr_lit(&w, ",\"name\":");
    wr_json_string(&w, hello->name, strlen(hello->name));
    wr_lit(&w, ",\"version\":1,\"supported_roles\":[\"player@v1\"]");
    if (hello->product_name || hello->manufacturer || hello->software_version) {
        wr_lit(&w, ",\"device_info\":{");
        bool comma = false;
        if (hello->product_name) {
            wr_lit(&w, "\"product_name\":");
            wr_json_string(&w, hello->product_name, strlen(hello->product_name));
            comma = true;
        }
        if (hello->manufacturer) {
            wr_lit(&w, comma ? ",\"manufacturer\":" : "\"manufacturer\":");
            wr_json_string(&w, hello->manufacturer, strlen(hello->manufacturer));
            comma = true;
        }
        if (hello->software_version) {
            wr_lit(&w, comma ? ",\"software_version\":" : "\"software_version\":");
            wr_json_string(&w, hello->software_version, strlen(hello->software_version));
        }
        wr_lit(&w, "}");
    }
    wr_lit(&w, ",\"player@v1_support\":{\"supported_formats\":[{\"codec\":");
    wr_json_string(&w, codec_name(EAF_SENDPIN_CODEC_PCM), 3);
    wr_lit(&w, ",\"channels\":");
    wr_u64(&w, hello->channels);
    wr_lit(&w, ",\"sample_rate\":");
    wr_u64(&w, hello->sample_rate);
    wr_lit(&w, ",\"bit_depth\":");
    wr_u64(&w, hello->bit_depth);
    wr_lit(&w, "}],\"buffer_capacity\":");
    wr_u64(&w, hello->buffer_capacity);
    wr_lit(&w, ",\"supported_commands\":[");
    if (hello->support_volume) {
        wr_lit(&w, "\"volume\"");
        if (hello->support_mute)
            wr_lit(&w, ",");
    }
    if (hello->support_mute)
        wr_lit(&w, "\"mute\"");
    wr_lit(&w, "]}");
    wr_lit(&w, "},\"type\":\"client/hello\"}");
    return finish(&w, written);
}

int eaf_sendspin_build_client_time(char *dst, size_t capacity, int64_t client_transmitted,
                                   size_t *written) {
    if (!dst || !written || !capacity)
        return EAF_INVALID;
    writer_t w = {dst, capacity, 0, true};
    wr_lit(&w, "{\"payload\":{\"client_transmitted\":");
    wr_i64(&w, client_transmitted);
    wr_lit(&w, "},\"type\":\"client/time\"}");
    return finish(&w, written);
}

int eaf_sendspin_build_client_state(char *dst, size_t capacity,
                                    const eaf_sendspin_client_state_t *state, size_t *written) {
    if (!dst || !state || !written || !capacity)
        return EAF_INVALID;
    writer_t w = {dst, capacity, 0, true};
    wr_lit(&w, "{\"payload\":{\"player\":{\"state\":\"synchronized\",\"volume\":");
    wr_i64(&w, state->volume);
    wr_lit(&w, ",\"muted\":");
    wr_lit(&w, state->muted ? "true" : "false");
    wr_lit(&w, ",\"static_delay_ms\":");
    wr_i64(&w, state->static_delay_ms);
    wr_lit(&w, ",\"required_lead_time_ms\":");
    wr_i64(&w, state->required_lead_time_ms);
    wr_lit(&w, ",\"min_buffer_ms\":");
    wr_i64(&w, state->min_buffer_ms);
    wr_lit(&w, "}},\"type\":\"client/state\"}");
    return finish(&w, written);
}

int eaf_sendspin_build_client_goodbye(char *dst, size_t capacity, const char *reason,
                                      size_t *written) {
    if (!dst || !reason || !written || !capacity)
        return EAF_INVALID;
    writer_t w = {dst, capacity, 0, true};
    wr_lit(&w, "{\"payload\":{\"reason\":");
    wr_json_string(&w, reason, strlen(reason));
    wr_lit(&w, "},\"type\":\"client/goodbye\"}");
    return finish(&w, written);
}
