#include "check.h"
#include <eaf/eaf_sendspin.h>
#include <string.h>

static uint8_t last_opcode;
static uint8_t last_payload[8192];
static size_t last_length;
static unsigned messages;

static int on_message(void *ctx, uint8_t opcode, const uint8_t *payload, size_t length) {
    (void)ctx;
    last_opcode = opcode;
    last_length = length;
    CHECK(length <= sizeof(last_payload));
    memcpy(last_payload, payload, length);
    ++messages;
    return 0;
}

static size_t server_frame(uint8_t *dst, uint8_t opcode, bool fin, const uint8_t *payload,
                           size_t length) {
    size_t position = 2;
    dst[0] = (uint8_t)((fin ? 0x80u : 0u) | opcode);
    if (length <= 125u) {
        dst[1] = (uint8_t)length;
    } else if (length <= 0xFFFFu) {
        dst[1] = 126;
        dst[2] = (uint8_t)(length >> 8);
        dst[3] = (uint8_t)length;
        position = 4;
    } else {
        dst[1] = 127;
        for (size_t i = 0; i < 8; ++i)
            dst[2 + i] = (uint8_t)((uint64_t)length >> (56u - 8u * i));
        position = 10;
    }
    memcpy(dst + position, payload, length);
    return position + length;
}

static void test_upgrade(void) {
    char buffer[256];
    size_t written = 0;
    CHECK(!eaf_sendspin_ws_build_upgrade(buffer, sizeof(buffer), "192.168.11.132:8927", "/sendspin",
                                         "BEfr8jM2J16+t47N8tiNHg==", &written));
    const char *expected = "GET /sendspin HTTP/1.1\r\nHost: 192.168.11.132:8927\r\n"
                           "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13"
                           "\r\nSec-WebSocket-Key: BEfr8jM2J16+t47N8tiNHg==\r\n\r\n";
    CHECK(written == strlen(expected) && !memcmp(buffer, expected, written));
    CHECK(eaf_sendspin_ws_build_upgrade(buffer, 8, "h", "/p", "k", &written) == EAF_INVALID);

    CHECK(!eaf_sendspin_ws_check_upgrade_response("HTTP/1.1 101 Switching Protocols\r\n\r\n", 38));
    CHECK(eaf_sendspin_ws_check_upgrade_response("HTTP/1.1 400 Bad Request\r\n\r\n", 30) ==
          EAF_INVALID);
    CHECK(eaf_sendspin_ws_check_upgrade_response("garbage", 7) == EAF_INVALID);
}

static void test_key(void) {
    eaf_sendspin_ws_prng_t a, b;
    eaf_sendspin_ws_prng_init(&a, 12345);
    eaf_sendspin_ws_prng_init(&b, 12345);
    char key_a[25], key_b[25];
    eaf_sendspin_ws_client_key(&a, key_a);
    eaf_sendspin_ws_client_key(&b, key_b);
    CHECK(!strcmp(key_a, key_b) && strlen(key_a) == 24);
    CHECK(key_a[22] == '=' && key_a[23] == '=');
    for (size_t i = 0; i < 22; ++i)
        CHECK((key_a[i] >= 'A' && key_a[i] <= 'Z') || (key_a[i] >= 'a' && key_a[i] <= 'z') ||
              (key_a[i] >= '0' && key_a[i] <= '9') || key_a[i] == '+' || key_a[i] == '/');
}

static void test_encode(void) {
    uint8_t frame[256];
    size_t written = 0;
    static const uint8_t payload[] = {'h', 'e', 'l', 'l', 'o'};
    CHECK(!eaf_sendspin_ws_encode(frame, sizeof(frame), EAF_SENDPIN_WS_TEXT, payload, 5,
                                  0x04030201u, &written));
    CHECK(written == 11u && frame[0] == 0x81u && frame[1] == 0x85u);
    CHECK(frame[2] == 0x01u && frame[3] == 0x02u && frame[4] == 0x03u && frame[5] == 0x04u);
    static const uint8_t masked[] = {0x69, 0x67, 0x6f, 0x68, 0x6e};
    CHECK(!memcmp(frame + 6, masked, 5));

    uint8_t large[512];
    memset(large, 0xAB, sizeof(large));
    CHECK(!eaf_sendspin_ws_encode(frame, sizeof(frame), EAF_SENDPIN_WS_BINARY, large, 200, 1u,
                                  &written));
    CHECK(written == 4u + 4u + 200u && frame[0] == 0x82u && frame[1] == 0xFEu &&
          frame[2] == 0x00u && frame[3] == 0xC8u);

    CHECK(eaf_sendspin_ws_encode(frame, 16u, EAF_SENDPIN_WS_BINARY, large, 200, 1u, &written) ==
          EAF_INVALID);
}

static void feed_all(eaf_sendspin_ws_rx_t *rx, const uint8_t *frame, size_t length,
                     eaf_sendspin_ws_message_fn callback) {
    for (size_t i = 0; i < length; ++i)
        CHECK(!eaf_sendspin_ws_rx_feed(rx, frame + i, 1, callback, NULL));
}

static void test_decode(void) {
    uint8_t scratch[8192];
    uint8_t frame[8192];
    eaf_sendspin_ws_rx_t rx;
    eaf_sendspin_ws_rx_init(&rx, scratch, sizeof(scratch));

    messages = 0;
    const char text[] = "{\"type\":\"server/hello\"}";
    size_t length =
        server_frame(frame, EAF_SENDPIN_WS_TEXT, true, (const uint8_t *)text, sizeof(text) - 1u);
    feed_all(&rx, frame, length, on_message);
    CHECK(messages == 1 && last_opcode == EAF_SENDPIN_WS_TEXT && last_length == sizeof(text) - 1u &&
          !memcmp(last_payload, text, last_length));

    static const uint8_t audio[300];
    length = server_frame(frame, EAF_SENDPIN_WS_BINARY, true, audio, sizeof(audio));
    feed_all(&rx, frame, length, on_message);
    CHECK(messages == 2 && last_opcode == EAF_SENDPIN_WS_BINARY && last_length == sizeof(audio));

    /* Masked server frame (unusual but valid). */
    uint8_t masked[32];
    masked[0] = 0x81u;
    masked[1] = (uint8_t)(0x80u | 5u);
    masked[2] = 0x11u;
    masked[3] = 0x22u;
    masked[4] = 0x33u;
    masked[5] = 0x44u;
    static const uint8_t payload[] = "hello";
    for (size_t i = 0; i < 5; ++i)
        masked[6 + i] = (uint8_t)(payload[i] ^ masked[2 + (i & 3u)]);
    feed_all(&rx, masked, 11, on_message);
    CHECK(messages == 3 && last_opcode == EAF_SENDPIN_WS_TEXT && last_length == 5 &&
          !memcmp(last_payload, "hello", 5));
}

static void test_fragmentation(void) {
    uint8_t scratch[64];
    uint8_t frame[64];
    eaf_sendspin_ws_rx_t rx;
    eaf_sendspin_ws_rx_init(&rx, scratch, sizeof(scratch));

    messages = 0;
    size_t first = server_frame(frame, EAF_SENDPIN_WS_TEXT, false, (const uint8_t *)"he", 2);
    feed_all(&rx, frame, first, on_message);
    CHECK(messages == 0);
    size_t second = server_frame(frame, 0x0u, true, (const uint8_t *)"llo", 3);
    feed_all(&rx, frame, second, on_message);
    CHECK(messages == 1 && last_opcode == EAF_SENDPIN_WS_TEXT && last_length == 5 &&
          !memcmp(last_payload, "hello", 5));

    /* Continuation without a start is a protocol error. */
    eaf_sendspin_ws_rx_init(&rx, scratch, sizeof(scratch));
    size_t bad = server_frame(frame, 0x0u, true, (const uint8_t *)"x", 1);
    CHECK(eaf_sendspin_ws_rx_feed(&rx, frame, bad, on_message, NULL) == EAF_INVALID);
}

static void test_control_and_limits(void) {
    uint8_t scratch[4];
    uint8_t frame[32];
    eaf_sendspin_ws_rx_t rx;
    eaf_sendspin_ws_rx_init(&rx, scratch, sizeof(scratch));

    messages = 0;
    size_t ping = server_frame(frame, EAF_SENDPIN_WS_PING, true, (const uint8_t *)"hi", 2);
    CHECK(!eaf_sendspin_ws_rx_feed(&rx, frame, ping, on_message, NULL));
    CHECK(messages == 1 && last_opcode == EAF_SENDPIN_WS_PING);

    size_t oversized =
        server_frame(frame, EAF_SENDPIN_WS_BINARY, true, (const uint8_t *)"12345", 5);
    CHECK(eaf_sendspin_ws_rx_feed(&rx, frame, oversized, on_message, NULL) == EAF_UNSUPPORTED);
}

int main(void) {
    test_upgrade();
    test_key();
    test_encode();
    test_decode();
    test_fragmentation();
    test_control_and_limits();
    puts("sendspin ws PASS");
    return 0;
}
