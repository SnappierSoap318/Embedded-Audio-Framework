#pragma once
#include <eaf/eaf_hal.h>
#define EAF_BT_PACKET_BYTES 1024u
#define EAF_BT_QUEUE_PACKETS 8u
typedef struct {
    uint16_t sequence, length;
    uint32_t timestamp;
    uint8_t frame_count;
    bool discontinuity;
    uint8_t data[EAF_BT_PACKET_BYTES];
} eaf_bt_packet_t;
typedef struct {
    eaf_bt_packet_t slots[EAF_BT_QUEUE_PACKETS];
    eaf_atomic_u32_t read_cursor, write_cursor;
    /* Producer-owned diagnostics; read after stopping callbacks. */
    uint32_t dropped;
    uint16_t expected_sequence;
    bool have_sequence, discontinuity;
} eaf_bt_ingress_t;
void eaf_bt_ingress_init(eaf_bt_ingress_t *q);
/* Single serialized BT callback producer. Pass SBC media header + payload,
   with RTP already removed. Fragmented SBC media is rejected. Never blocks.
   RTP sequence/timestamp are retained; overflow drops the entire new packet. */
int eaf_bt_sbc_receive(eaf_bt_ingress_t *q, uint16_t sequence, uint32_t timestamp,
                       const uint8_t *payload, size_t length);
/* Single decode worker consumer; copies packet into caller-owned scratch. */
bool eaf_bt_ingress_pop(eaf_bt_ingress_t *q, eaf_bt_packet_t *out);
