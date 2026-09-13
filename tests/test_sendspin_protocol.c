#include "check.h"
#include <eaf/eaf_sendspin.h>
#include <string.h>

/* Exact captured wire strings (docs/bench/sendspin-capture-2026-09-13.md). */
static const char server_hello_json[] =
    "{\"payload\":{\"server_id\":\"Jf0D-_vdN-lqYT93AUwhRk9Ucg9sAZxSOSW-ohaFaD0\",\"name\":"
    "\"Music Assistant (snappy-storage)\",\"version\":1,\"connection_reason\":\"discovery\","
    "\"active_roles\":[\"player@v1\"]},\"type\":\"server/hello\"}";
static const char server_time_json[] =
    "{\"payload\":{\"client_transmitted\":73611376768,\"server_received\":61896583899,"
    "\"server_transmitted\":61896583986},\"type\":\"server/time\"}";
static const char stream_start_json[] =
    "{\"payload\":{\"server_transmitted\":61991839289,\"player\":{\"codec\":\"pcm\","
    "\"sample_rate\":44100,\"channels\":2,\"bit_depth\":16}},\"type\":\"stream/start\"}";
static const char stream_end_json[] =
    "{\"payload\":{\"server_transmitted\":62004590553,\"roles\":[\"player\"]},"
    "\"type\":\"stream/end\"}";
static const char group_update_json[] =
    "{\"payload\":{\"playback_state\":\"playing\",\"group_id\":"
    "\"4c3ab809-cb14-4797-a686-4c9af00bff05\"},\"type\":\"group/update\"}";
static const char server_command_json[] =
    "{\"payload\":{\"player\":{\"command\":\"volume\",\"volume\":42}},"
    "\"type\":\"server/command\"}";
static const char client_hello_wire[] =
    "{\"payload\":{\"client_id\":\"eaf-phase0-probe\",\"name\":\"EAF Phase0 "
    "Probe\",\"version\":1,\"supported_roles\":[\"player@v1\"],\"device_info\":{\"product_name\":"
    "\"EAF Phase0 Probe\",\"manufacturer\":\"EAF\",\"software_version\":\"phase0\"},"
    "\"player@v1_support\":{\"supported_formats\":[{\"codec\":\"pcm\",\"channels\":2,"
    "\"sample_rate\":44100,\"bit_depth\":16}],\"buffer_capacity\":262144,"
    "\"supported_commands\":[\"volume\",\"mute\"]}},\"type\":\"client/hello\"}";
static const char client_time_wire[] =
    "{\"payload\":{\"client_transmitted\":73611376768},\"type\":\"client/time\"}";
static const char client_state_wire[] =
    "{\"payload\":{\"player\":{\"state\":\"synchronized\",\"volume\":100,\"muted\":false,"
    "\"static_delay_ms\":0,\"required_lead_time_ms\":250,\"min_buffer_ms\":250}},"
    "\"type\":\"client/state\"}";
static const char client_goodbye_wire[] =
    "{\"payload\":{\"reason\":\"user_request\"},\"type\":\"client/goodbye\"}";

static void check_wire(const char *dst, size_t written, const char *expected) {
    CHECK(written == strlen(expected));
    CHECK(!memcmp(dst, expected, written));
}

static void parse_messages(void) {
    eaf_sendspin_server_hello_t hello;
    CHECK(!eaf_sendspin_parse_server_hello(server_hello_json, strlen(server_hello_json), &hello));
    CHECK(hello.player_active && hello.version == 1);
    CHECK(hello.server_id_length == 43 && !memcmp(hello.server_id, "Jf0D-", 5));
    CHECK(hello.name_length == strlen("Music Assistant (snappy-storage)"));
    CHECK(hello.connection_reason_length == strlen("discovery"));

    eaf_sendspin_server_time_t now;
    CHECK(!eaf_sendspin_parse_server_time(server_time_json, strlen(server_time_json), &now));
    CHECK(now.client_transmitted == 73611376768LL && now.server_received == 61896583899LL &&
          now.server_transmitted == 61896583986LL);

    eaf_sendspin_stream_start_t start;
    CHECK(!eaf_sendspin_parse_stream_start(stream_start_json, strlen(stream_start_json), &start));
    CHECK(start.codec == EAF_SENDPIN_CODEC_PCM && start.sample_rate == 44100);
    CHECK(start.channels == 2 && start.bit_depth == 16);
    CHECK(start.server_transmitted == 61991839289LL && start.codec_header == NULL);

    eaf_sendspin_stream_end_t end;
    CHECK(!eaf_sendspin_parse_stream_end(stream_end_json, strlen(stream_end_json), &end));
    CHECK(end.end_player && end.server_transmitted == 62004590553LL);

    eaf_sendspin_group_update_t group;
    CHECK(!eaf_sendspin_parse_group_update(group_update_json, strlen(group_update_json), &group));
    CHECK(group.playback_state_length == 7 && !memcmp(group.playback_state, "playing", 7));
    CHECK(group.group_id_length == 36);

    eaf_sendspin_server_command_t command;
    CHECK(!eaf_sendspin_parse_server_command(server_command_json, strlen(server_command_json),
                                             &command));
    CHECK(command.command == EAF_SENDPIN_COMMAND_VOLUME && command.volume == 42);

    /* Optional fields default. */
    static const char mute_json[] =
        "{\"payload\":{\"player\":{\"command\":\"mute\",\"mute\":true}},\"type\":"
        "\"server/command\"}";
    CHECK(!eaf_sendspin_parse_server_command(mute_json, strlen(mute_json), &command));
    CHECK(command.command == EAF_SENDPIN_COMMAND_MUTE && command.muted && command.volume == -1);
}

static void json_reader(void) {
    const char *json = "{\"type\":\"stream/start\",\"payload\":{\"player\":{\"codec\":\"flac\","
                       "\"bit_depth\":24},\"roles\":[\"player\",\"artwork\"],\"n\":-12}}";
    size_t length = strlen(json);
    eaf_sendspin_json_value_t value;
    CHECK(!eaf_sendspin_json_get(json, length, "type", &value) &&
          eaf_sendspin_json_string_equals(&value, "stream/start"));
    CHECK(!eaf_sendspin_json_get(json, length, "payload.player.bit_depth", &value) &&
          value.type == EAF_SENDPIN_JSON_NUMBER && value.number == 24);
    CHECK(!eaf_sendspin_json_get(json, length, "payload.n", &value) && value.number == -12);
    CHECK(!eaf_sendspin_json_get(json, length, "payload.roles", &value) &&
          eaf_sendspin_json_array_contains(&value, "player"));
    CHECK(!eaf_sendspin_json_get(json, length, "payload.roles", &value) &&
          !eaf_sendspin_json_array_contains(&value, "visualizer"));
    CHECK(eaf_sendspin_json_get(json, length, "payload.missing", &value) != 0);

    /* Whitespace, nesting and an escaped quote in a string. */
    const char *pretty =
        "{\n  \"payload\": {\"name\": \"a\\\"b\", \"list\": [1, {\"x\": 2}, \"z\"]},\n"
        "  \"type\": \"server/hello\"\n}";
    CHECK(!eaf_sendspin_json_get(pretty, strlen(pretty), "payload.list", &value) &&
          value.type == EAF_SENDPIN_JSON_ARRAY && eaf_sendspin_json_array_contains(&value, "z"));
}

static void build_messages(void) {
    char buffer[512];
    size_t written = 0;
    eaf_sendspin_client_hello_t hello = {.client_id = "eaf-phase0-probe",
                                         .name = "EAF Phase0 Probe",
                                         .product_name = "EAF Phase0 Probe",
                                         .manufacturer = "EAF",
                                         .software_version = "phase0",
                                         .sample_rate = 44100,
                                         .channels = 2,
                                         .bit_depth = 16,
                                         .buffer_capacity = 262144,
                                         .support_volume = true,
                                         .support_mute = true};
    CHECK(!eaf_sendspin_build_client_hello(buffer, sizeof(buffer), &hello, &written));
    check_wire(buffer, written, client_hello_wire);

    CHECK(!eaf_sendspin_build_client_time(buffer, sizeof(buffer), 73611376768LL, &written));
    check_wire(buffer, written, client_time_wire);

    eaf_sendspin_client_state_t state = {
        .volume = 100, .muted = false, .static_delay_ms = 0, .required_lead_time_ms = 250};
    state.min_buffer_ms = 250;
    CHECK(!eaf_sendspin_build_client_state(buffer, sizeof(buffer), &state, &written));
    check_wire(buffer, written, client_state_wire);

    CHECK(!eaf_sendspin_build_client_goodbye(buffer, sizeof(buffer), "user_request", &written));
    check_wire(buffer, written, client_goodbye_wire);

    /* Optional device info and commands are omitted when absent. */
    eaf_sendspin_client_hello_t minimal = {.client_id = "id",
                                           .name = "n",
                                           .sample_rate = 48000,
                                           .channels = 2,
                                           .bit_depth = 16,
                                           .buffer_capacity = 1};
    CHECK(!eaf_sendspin_build_client_hello(buffer, sizeof(buffer), &minimal, &written));
    buffer[written] = '\0';
    CHECK(!strstr(buffer, "device_info") && !strstr(buffer, "volume"));
    CHECK(strstr(buffer, "\"supported_commands\":[]") != NULL);

    /* Escaping and truncation guard. */
    eaf_sendspin_client_hello_t quoted = minimal;
    quoted.name = "a\"b\\c\n";
    CHECK(!eaf_sendspin_build_client_hello(buffer, sizeof(buffer), &quoted, &written));
    buffer[written] = '\0';
    CHECK(strstr(buffer, "\"name\":\"a\\\"b\\\\c\\n\"") != NULL);
    CHECK(eaf_sendspin_build_client_hello(buffer, 8, &hello, &written) == EAF_INVALID);
    CHECK(eaf_sendspin_build_client_time(NULL, 0, 0, &written) == EAF_INVALID);
}

int main(void) {
    parse_messages();
    json_reader();
    build_messages();
    puts("sendspin protocol PASS");
    return 0;
}
