#include <eaf/eaf_bytes.h>
#include <eaf/eaf_sendspin.h>
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
    const char *p = c->p;
    bool negative = false;
    if (p < c->end && *p == '-') {
        negative = true;
        ++p;
    }
    if (p >= c->end || *p < '0' || *p > '9')
        return EAF_INVALID;
    uint64_t magnitude = 0;
    bool overflow = false;
    while (p < c->end && *p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p - '0');
        if (!overflow) {
            if (magnitude > (UINT64_MAX - digit) / 10u)
                overflow = true;
            else
                magnitude = magnitude * 10u + digit;
        }
        ++p;
    }
    if (p < c->end && (*p == '.' || *p == 'e' || *p == 'E'))
        return EAF_INVALID;
    c->p = p;
    if (negative) {
        if (overflow || magnitude > (uint64_t)INT64_MAX + 1u)
            *out = INT64_MIN;
        else
            *out = (int64_t)(0u - magnitude);
    } else if (overflow || magnitude > (uint64_t)INT64_MAX) {
        *out = INT64_MAX;
    } else {
        *out = (int64_t)magnitude;
    }
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

static int parse_array_membership(cursor_t *c, const char *needle, size_t needle_length,
                                  bool *member) {
    skip_ws(c);
    if (c->p >= c->end || *c->p != '[') {
        *member = false;
        return skip_value(c);
    }
    ++c->p;
    skip_ws(c);
    if (c->p < c->end && *c->p == ']') {
        ++c->p;
        return EAF_OK;
    }
    for (;;) {
        skip_ws(c);
        if (c->p < c->end && *c->p == '"') {
            const char *element;
            size_t element_length;
            int rc = scan_string(c, &element, &element_length);
            if (rc)
                return rc;
            if (!*member && element_length == needle_length &&
                !memcmp(element, needle, needle_length))
                *member = true;
        } else {
            int rc = skip_value(c);
            if (rc)
                return rc;
        }
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

typedef struct {
    const char *key;
    size_t key_length;
    const char *needle;
    size_t needle_length;
    eaf_sendspin_json_value_t *value;
    bool member;
    bool found;
} json_field_t;

static int scan_fields(cursor_t *c, json_field_t *fields, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (fields[i].value)
            *fields[i].value = (eaf_sendspin_json_value_t){0};
    }
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
            return EAF_OK;
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
        json_field_t *match = NULL;
        for (size_t i = 0; i < count; ++i) {
            if (!fields[i].found && fields[i].key_length == key_length &&
                !memcmp(key, fields[i].key, key_length)) {
                match = &fields[i];
                break;
            }
        }
        if (!match) {
            rc = skip_value(c);
            if (rc)
                return rc;
        } else {
            match->found = true;
            if (match->needle) {
                rc = parse_array_membership(c, match->needle, match->needle_length, &match->member);
            } else {
                rc = parse_value(c, match->value);
            }
            if (rc)
                return rc;
        }
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
    cursor_t c = {array->raw, array->raw + array->raw_length};
    bool member = false;
    (void)parse_array_membership(&c, needle, strlen(needle), &member);
    return member ? 1 : 0;
}

int eaf_sendspin_json_string_equals(const eaf_sendspin_json_value_t *value, const char *text) {
    size_t n = strlen(text);
    return value && value->type == EAF_SENDPIN_JSON_STRING && value->string_length == n &&
           !memcmp(value->string, text, n);
}

static eaf_sendspin_codec_t codec_from_value(const eaf_sendspin_json_value_t *value) {
    if (eaf_sendspin_json_string_equals(value, "pcm"))
        return EAF_SENDPIN_CODEC_PCM;
    if (eaf_sendspin_json_string_equals(value, "flac"))
        return EAF_SENDPIN_CODEC_FLAC;
    if (eaf_sendspin_json_string_equals(value, "opus"))
        return EAF_SENDPIN_CODEC_OPUS;
    if (eaf_sendspin_json_string_equals(value, "mp3"))
        return EAF_SENDPIN_CODEC_MP3;
    if (eaf_sendspin_json_string_equals(value, "vorbis"))
        return EAF_SENDPIN_CODEC_VORBIS;
    return EAF_SENDPIN_CODEC_UNKNOWN;
}

int eaf_sendspin_parse_server_hello(const char *json, size_t length,
                                    eaf_sendspin_server_hello_t *out) {
    if (!json || !out)
        return EAF_INVALID;
    *out = (eaf_sendspin_server_hello_t){0};
    eaf_sendspin_json_value_t payload;
    if (eaf_sendspin_json_get(json, length, "payload", &payload) ||
        payload.type != EAF_SENDPIN_JSON_OBJECT)
        return EAF_INVALID;
    eaf_sendspin_json_value_t server_id, name, connection_reason, version;
    json_field_t fields[] = {
        {.key = "active_roles",
         .key_length = sizeof("active_roles") - 1u,
         .needle = "player@v1",
         .needle_length = sizeof("player@v1") - 1u},
        {.key = "server_id", .key_length = sizeof("server_id") - 1u, .value = &server_id},
        {.key = "name", .key_length = sizeof("name") - 1u, .value = &name},
        {.key = "connection_reason",
         .key_length = sizeof("connection_reason") - 1u,
         .value = &connection_reason},
        {.key = "version", .key_length = sizeof("version") - 1u, .value = &version},
    };
    cursor_t c = {payload.raw, payload.raw + payload.raw_length};
    if (scan_fields(&c, fields, sizeof(fields) / sizeof(fields[0])))
        return EAF_INVALID;
    if (!fields[0].found)
        return EAF_INVALID;
    out->player_active = fields[0].member;
    if (fields[1].found && server_id.type == EAF_SENDPIN_JSON_STRING) {
        out->server_id = server_id.string;
        out->server_id_length = server_id.string_length;
    }
    if (fields[2].found && name.type == EAF_SENDPIN_JSON_STRING) {
        out->name = name.string;
        out->name_length = name.string_length;
    }
    if (fields[3].found && connection_reason.type == EAF_SENDPIN_JSON_STRING) {
        out->connection_reason = connection_reason.string;
        out->connection_reason_length = connection_reason.string_length;
    }
    if (fields[4].found && version.type == EAF_SENDPIN_JSON_NUMBER)
        out->version = (uint32_t)version.number;
    return EAF_OK;
}

int eaf_sendspin_parse_server_time(const char *json, size_t length,
                                   eaf_sendspin_server_time_t *out) {
    if (!json || !out)
        return EAF_INVALID;
    eaf_sendspin_json_value_t payload;
    if (eaf_sendspin_json_get(json, length, "payload", &payload) ||
        payload.type != EAF_SENDPIN_JSON_OBJECT)
        return EAF_INVALID;
    eaf_sendspin_json_value_t client_transmitted, server_received, server_transmitted;
    json_field_t fields[] = {
        {.key = "client_transmitted",
         .key_length = sizeof("client_transmitted") - 1u,
         .value = &client_transmitted},
        {.key = "server_received",
         .key_length = sizeof("server_received") - 1u,
         .value = &server_received},
        {.key = "server_transmitted",
         .key_length = sizeof("server_transmitted") - 1u,
         .value = &server_transmitted},
    };
    cursor_t c = {payload.raw, payload.raw + payload.raw_length};
    if (scan_fields(&c, fields, sizeof(fields) / sizeof(fields[0])))
        return EAF_INVALID;
    if (client_transmitted.type != EAF_SENDPIN_JSON_NUMBER ||
        server_received.type != EAF_SENDPIN_JSON_NUMBER ||
        server_transmitted.type != EAF_SENDPIN_JSON_NUMBER)
        return EAF_INVALID;
    out->client_transmitted = client_transmitted.number;
    out->server_received = server_received.number;
    out->server_transmitted = server_transmitted.number;
    return EAF_OK;
}

int eaf_sendspin_parse_stream_start(const char *json, size_t length,
                                    eaf_sendspin_stream_start_t *out) {
    if (!json || !out)
        return EAF_INVALID;
    *out = (eaf_sendspin_stream_start_t){0};
    eaf_sendspin_json_value_t payload;
    if (eaf_sendspin_json_get(json, length, "payload", &payload) ||
        payload.type != EAF_SENDPIN_JSON_OBJECT)
        return EAF_INVALID;
    eaf_sendspin_json_value_t player, transmitted;
    json_field_t payload_fields[] = {
        {.key = "player", .key_length = sizeof("player") - 1u, .value = &player},
        {.key = "server_transmitted",
         .key_length = sizeof("server_transmitted") - 1u,
         .value = &transmitted},
    };
    cursor_t pc = {payload.raw, payload.raw + payload.raw_length};
    if (scan_fields(&pc, payload_fields, sizeof(payload_fields) / sizeof(payload_fields[0])) ||
        !payload_fields[0].found || player.type != EAF_SENDPIN_JSON_OBJECT)
        return EAF_INVALID;
    eaf_sendspin_json_value_t codec, sample_rate, channels, bit_depth, codec_header;
    json_field_t player_fields[] = {
        {.key = "codec", .key_length = sizeof("codec") - 1u, .value = &codec},
        {.key = "sample_rate", .key_length = sizeof("sample_rate") - 1u, .value = &sample_rate},
        {.key = "channels", .key_length = sizeof("channels") - 1u, .value = &channels},
        {.key = "bit_depth", .key_length = sizeof("bit_depth") - 1u, .value = &bit_depth},
        {.key = "codec_header", .key_length = sizeof("codec_header") - 1u, .value = &codec_header},
    };
    cursor_t cc = {player.raw, player.raw + player.raw_length};
    if (scan_fields(&cc, player_fields, sizeof(player_fields) / sizeof(player_fields[0])))
        return EAF_INVALID;
    if (!player_fields[0].found)
        return EAF_INVALID;
    out->codec = codec_from_value(&codec);
    if (sample_rate.type != EAF_SENDPIN_JSON_NUMBER || channels.type != EAF_SENDPIN_JSON_NUMBER ||
        bit_depth.type != EAF_SENDPIN_JSON_NUMBER)
        return EAF_INVALID;
    out->sample_rate = (uint32_t)sample_rate.number;
    out->channels = (uint8_t)channels.number;
    out->bit_depth = (uint8_t)bit_depth.number;
    if (transmitted.type == EAF_SENDPIN_JSON_NUMBER)
        out->server_transmitted = transmitted.number;
    if (codec_header.type == EAF_SENDPIN_JSON_STRING) {
        out->codec_header = codec_header.string;
        out->codec_header_length = codec_header.string_length;
    }
    return EAF_OK;
}

int eaf_sendspin_parse_stream_end(const char *json, size_t length, eaf_sendspin_stream_end_t *out) {
    if (!json || !out)
        return EAF_INVALID;
    *out = (eaf_sendspin_stream_end_t){0};
    eaf_sendspin_json_value_t payload;
    int prc = eaf_sendspin_json_get(json, length, "payload", &payload);
    if (prc == EAF_UNSUPPORTED) {
        out->end_player = true;
        return EAF_OK;
    }
    if (prc || payload.type != EAF_SENDPIN_JSON_OBJECT)
        return EAF_INVALID;
    eaf_sendspin_json_value_t transmitted;
    json_field_t fields[] = {
        {.key = "server_transmitted",
         .key_length = sizeof("server_transmitted") - 1u,
         .value = &transmitted},
        {.key = "roles",
         .key_length = sizeof("roles") - 1u,
         .needle = "player",
         .needle_length = sizeof("player") - 1u},
    };
    cursor_t c = {payload.raw, payload.raw + payload.raw_length};
    if (scan_fields(&c, fields, sizeof(fields) / sizeof(fields[0])))
        return EAF_INVALID;
    if (transmitted.type == EAF_SENDPIN_JSON_NUMBER)
        out->server_transmitted = transmitted.number;
    out->end_player = !fields[1].found || fields[1].member;
    return EAF_OK;
}

int eaf_sendspin_parse_group_update(const char *json, size_t length,
                                    eaf_sendspin_group_update_t *out) {
    if (!json || !out)
        return EAF_INVALID;
    *out = (eaf_sendspin_group_update_t){0};
    eaf_sendspin_json_value_t payload;
    if (eaf_sendspin_json_get(json, length, "payload", &payload) ||
        payload.type != EAF_SENDPIN_JSON_OBJECT)
        return EAF_OK;
    eaf_sendspin_json_value_t playback_state, group_id;
    json_field_t fields[] = {
        {.key = "playback_state",
         .key_length = sizeof("playback_state") - 1u,
         .value = &playback_state},
        {.key = "group_id", .key_length = sizeof("group_id") - 1u, .value = &group_id},
    };
    cursor_t c = {payload.raw, payload.raw + payload.raw_length};
    if (scan_fields(&c, fields, sizeof(fields) / sizeof(fields[0])))
        return EAF_OK;
    if (playback_state.type == EAF_SENDPIN_JSON_STRING) {
        out->playback_state = playback_state.string;
        out->playback_state_length = playback_state.string_length;
    }
    if (group_id.type == EAF_SENDPIN_JSON_STRING) {
        out->group_id = group_id.string;
        out->group_id_length = group_id.string_length;
    }
    return EAF_OK;
}

int eaf_sendspin_parse_server_command(const char *json, size_t length,
                                      eaf_sendspin_server_command_t *out) {
    if (!json || !out)
        return EAF_INVALID;
    *out = (eaf_sendspin_server_command_t){
        .command = EAF_SENDPIN_COMMAND_UNKNOWN, .volume = -1, .static_delay_ms = -1};
    eaf_sendspin_json_value_t payload;
    if (eaf_sendspin_json_get(json, length, "payload", &payload) ||
        payload.type != EAF_SENDPIN_JSON_OBJECT)
        return EAF_INVALID;
    eaf_sendspin_json_value_t player, command, volume, mute, static_delay;
    json_field_t payload_fields[] = {
        {.key = "player", .key_length = sizeof("player") - 1u, .value = &player},
    };
    cursor_t pc = {payload.raw, payload.raw + payload.raw_length};
    if (scan_fields(&pc, payload_fields, sizeof(payload_fields) / sizeof(payload_fields[0])) ||
        !payload_fields[0].found || player.type != EAF_SENDPIN_JSON_OBJECT)
        return EAF_INVALID;
    json_field_t player_fields[] = {
        {.key = "command", .key_length = sizeof("command") - 1u, .value = &command},
        {.key = "volume", .key_length = sizeof("volume") - 1u, .value = &volume},
        {.key = "mute", .key_length = sizeof("mute") - 1u, .value = &mute},
        {.key = "static_delay_ms",
         .key_length = sizeof("static_delay_ms") - 1u,
         .value = &static_delay},
    };
    cursor_t cc = {player.raw, player.raw + player.raw_length};
    if (scan_fields(&cc, player_fields, sizeof(player_fields) / sizeof(player_fields[0])))
        return EAF_INVALID;
    if (!player_fields[0].found)
        return EAF_INVALID;
    if (eaf_sendspin_json_string_equals(&command, "volume"))
        out->command = EAF_SENDPIN_COMMAND_VOLUME;
    else if (eaf_sendspin_json_string_equals(&command, "mute"))
        out->command = EAF_SENDPIN_COMMAND_MUTE;
    else if (eaf_sendspin_json_string_equals(&command, "set_static_delay"))
        out->command = EAF_SENDPIN_COMMAND_SET_STATIC_DELAY;
    if (volume.type == EAF_SENDPIN_JSON_NUMBER)
        out->volume = (int32_t)volume.number;
    if (mute.type == EAF_SENDPIN_JSON_BOOL)
        out->muted = mute.boolean;
    if (static_delay.type == EAF_SENDPIN_JSON_NUMBER)
        out->static_delay_ms = (int32_t)static_delay.number;
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
    if (!w->ok)
        return;
    size_t n = eaf_bytes_write_u64_dec(w->buf + w->used, w->capacity - w->used, value);
    if (!n) {
        w->ok = false;
        return;
    }
    w->used += n;
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

/* Returns NULL for codecs that must not be advertised. */
static const char *codec_name(eaf_sendspin_codec_t codec) {
    switch (codec) {
    case EAF_SENDPIN_CODEC_PCM:
        return "pcm";
    case EAF_SENDPIN_CODEC_FLAC:
        return "flac";
    case EAF_SENDPIN_CODEC_OPUS:
        return "opus";
    case EAF_SENDPIN_CODEC_MP3:
        return "mp3";
    case EAF_SENDPIN_CODEC_VORBIS:
        return "vorbis";
    default:
        return NULL;
    }
}

static void write_format(writer_t *w, const eaf_sendspin_format_t *format) {
    const char *name = codec_name(format->codec);
    wr_lit(w, "{\"codec\":");
    wr_json_string(w, name, strlen(name));
    wr_lit(w, ",\"channels\":");
    wr_u64(w, format->channels);
    wr_lit(w, ",\"sample_rate\":");
    wr_u64(w, format->sample_rate);
    /* Precision is a PCM property; compressed formats carry it in-band. */
    if (format->codec == EAF_SENDPIN_CODEC_PCM && format->bit_depth != 0u) {
        wr_lit(w, ",\"bit_depth\":");
        wr_u64(w, format->bit_depth);
    }
    wr_lit(w, "}");
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
    wr_lit(&w, ",\"player@v1_support\":{\"supported_formats\":[");
    size_t emitted = 0;
    for (size_t i = 0; hello->formats && i < hello->format_count; ++i) {
        if (!codec_name(hello->formats[i].codec))
            continue;
        if (emitted++)
            wr_char(&w, ',');
        write_format(&w, &hello->formats[i]);
    }
    if (emitted == 0u) {
        const eaf_sendspin_format_t fallback = {.codec = EAF_SENDPIN_CODEC_PCM,
                                                .sample_rate = hello->sample_rate,
                                                .channels = hello->channels,
                                                .bit_depth = hello->bit_depth};
        write_format(&w, &fallback);
    }
    wr_lit(&w, "],\"buffer_capacity\":");
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
                                    const eaf_sendspin_player_state_t *state, size_t *written) {
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
