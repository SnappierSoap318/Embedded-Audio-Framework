#include "check.h"
#include <eaf/eaf_bt.h>
#include <eaf/eaf_lms.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
static unsigned packets;
static int callback(void *ctx, const uint8_t *p, size_t len) {
    (void)ctx;
    CHECK(len == 4 && !memcmp(p, "aude", 4));
    ++packets;
    return EAF_OK;
}
static int fail_callback(void *ctx, const uint8_t *p, size_t len) {
    (void)ctx;
    (void)p;
    (void)len;
    return EAF_IO;
}
static eaf_bt_ingress_t queue;
static void *produce(void *ctx) {
    (void)ctx;
    for (uint32_t i = 0; i < 100000; ++i) {
        uint8_t data[] = {1, (uint8_t)i, (uint8_t)(i >> 8)};
        while (eaf_bt_sbc_receive(&queue, (uint16_t)i, i, data, sizeof(data)))
            (void)sched_yield();
    }
    return NULL;
}
int main(void) {
    static eaf_lms_parser_t parser;
    uint8_t frames[] = {0, 4, 'a', 'u', 'd', 'e', 0, 4, 'a', 'u', 'd', 'e'};
    for (size_t split = 0; split <= sizeof(frames); ++split) {
        packets = 0;
        eaf_lms_parser_init(&parser, callback, NULL);
        CHECK(eaf_lms_feed(&parser, frames, split) == 0);
        CHECK(eaf_lms_feed(&parser, frames + split, sizeof(frames) - split) == 0);
        CHECK(packets == 2);
    }
    eaf_lms_parser_init(&parser, callback, NULL);
    const uint8_t huge[] = {0xff, 0xff};
    CHECK(eaf_lms_feed(&parser, huge, 2) == EAF_INVALID);
    CHECK(eaf_lms_feed(&parser, frames, 6) == EAF_STATE);
    eaf_lms_parser_init(&parser, callback, NULL);
    const uint8_t tiny[] = {0, 3};
    CHECK(eaf_lms_feed(&parser, tiny, 2) == EAF_INVALID);
    eaf_lms_parser_init(&parser, fail_callback, NULL);
    CHECK(eaf_lms_feed(&parser, frames, 6) == EAF_IO && parser.failed);
    uint8_t strm[32] = {'s', 't', 'r', 'm', 's', '1', 'p', '1', '3', '2', '1'};
    strm[22] = 0x23;
    strm[23] = 0x28;
    strm[24] = 192;
    strm[25] = 168;
    strm[26] = 1;
    strm[27] = 2;
    memcpy(strm + 28, "GET ", 4);
    eaf_lms_stream_t stream;
    CHECK(eaf_lms_parse_stream(strm, 27, &stream) == EAF_INVALID);
    CHECK(eaf_lms_parse_stream(strm, 32, &stream) == 0);
    CHECK(stream.command == 's' && stream.codec == 'p' && stream.server_port == 9000);
    CHECK(stream.server_ipv4 == 0xc0a80102u && stream.request_length == 4);
    CHECK(!memcmp(stream.request, "GET ", 4));
    uint8_t helo[128], mac[] = {2, 0, 0, 0, 0, 1};
    size_t written;
    CHECK(eaf_lms_helo(helo, sizeof(helo), mac, "Model=eaf", &written) == 0);
    CHECK(written == 53 && !memcmp(helo, "HELO", 4) && helo[7] == 45 && helo[8] == 12);
    CHECK(!memcmp(helo + 10, mac, 6) && !memcmp(helo + 44, "Model=eaf", 9));
    CHECK(eaf_lms_helo(helo, 10, mac, "", &written) == EAF_INVALID && !written);
    eaf_bt_ingress_init(&queue);
    uint8_t media[] = {1, 0x9c, 1, 2, 3};
    static eaf_bt_packet_t out;
    CHECK(eaf_bt_sbc_receive(&queue, 65535, 123, media, 5) == 0);
    media[1] = 0;
    CHECK(eaf_bt_ingress_pop(&queue, &out) && out.data[0] == 0x9c && !out.discontinuity);
    CHECK(eaf_bt_sbc_receive(&queue, 0, 124, media, 5) == 0);
    CHECK(eaf_bt_ingress_pop(&queue, &out) && !out.discontinuity);
    for (unsigned i = 0; i < EAF_BT_QUEUE_PACKETS; ++i)
        CHECK(eaf_bt_sbc_receive(&queue, (uint16_t)(i + 1u), i, media, 5) == 0);
    CHECK(eaf_bt_sbc_receive(&queue, 9, 0, media, 5) == EAF_IO);
    for (unsigned i = 0; i < EAF_BT_QUEUE_PACKETS; ++i)
        CHECK(eaf_bt_ingress_pop(&queue, &out));
    CHECK(eaf_bt_sbc_receive(&queue, 10, 0, media, 5) == 0);
    CHECK(eaf_bt_ingress_pop(&queue, &out) && out.discontinuity);
    media[0] = 0x81;
    CHECK(eaf_bt_sbc_receive(&queue, 11, 0, media, 5) == EAF_INVALID);
    media[0] = 0;
    CHECK(eaf_bt_sbc_receive(&queue, 12, 0, media, 5) == EAF_INVALID);
    CHECK(queue.dropped == 3);
    eaf_bt_ingress_init(&queue);
    hal_atomic_set(&queue.read_cursor, UINT32_MAX - 3u);
    hal_atomic_set(&queue.write_cursor, UINT32_MAX - 3u);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, produce, NULL) == 0);
    for (uint32_t i = 0; i < 100000; ++i) {
        while (!eaf_bt_ingress_pop(&queue, &out))
            (void)sched_yield();
        CHECK(out.timestamp == i && out.sequence == (uint16_t)i && out.length == 2);
        CHECK(out.data[0] == (uint8_t)i && out.data[1] == (uint8_t)(i >> 8));
    }
    CHECK(pthread_join(thread, NULL) == 0);
    return 0;
}
