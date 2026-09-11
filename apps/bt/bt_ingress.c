#include <eaf/eaf_bt.h>
#include <string.h>
void eaf_bt_ingress_init(eaf_bt_ingress_t *q) {
    *q = (eaf_bt_ingress_t){0};
    atomic_init(&q->read_cursor.value, 0u);
    atomic_init(&q->write_cursor.value, 0u);
}
int eaf_bt_sbc_receive(eaf_bt_ingress_t *q, uint16_t sequence, uint32_t timestamp,
                       const uint8_t *payload, size_t length) {
    if (!q)
        return EAF_INVALID;
    if (!payload || length < 2u || length - 1u > EAF_BT_PACKET_BYTES || (payload[0] & 0xf0u) ||
        !(payload[0] & 0x0fu)) {
        ++q->dropped;
        q->discontinuity = true;
        return EAF_INVALID;
    }
    bool gap = q->have_sequence && sequence != q->expected_sequence;
    q->expected_sequence = (uint16_t)(sequence + 1u);
    q->have_sequence = true;
    uint32_t write = hal_atomic_get(&q->write_cursor);
    if (write - hal_atomic_get(&q->read_cursor) == EAF_BT_QUEUE_PACKETS) {
        ++q->dropped;
        q->discontinuity = true;
        return EAF_IO;
    }
    eaf_bt_packet_t *p = &q->slots[write % EAF_BT_QUEUE_PACKETS];
    p->sequence = sequence;
    p->timestamp = timestamp;
    p->length = (uint16_t)(length - 1u);
    p->frame_count = payload[0] & 0x0fu;
    p->discontinuity = gap || q->discontinuity;
    memcpy(p->data, payload + 1, length - 1u);
    q->discontinuity = false;
    hal_atomic_set(&q->write_cursor, write + 1u);
    return EAF_OK;
}
bool eaf_bt_ingress_pop(eaf_bt_ingress_t *q, eaf_bt_packet_t *out) {
    if (!q || !out)
        return false;
    uint32_t read = hal_atomic_get(&q->read_cursor);
    if (read == hal_atomic_get(&q->write_cursor))
        return false;
    const eaf_bt_packet_t *p = &q->slots[read % EAF_BT_QUEUE_PACKETS];
    out->sequence = p->sequence;
    out->timestamp = p->timestamp;
    out->length = p->length;
    out->frame_count = p->frame_count;
    out->discontinuity = p->discontinuity;
    memcpy(out->data, p->data, p->length);
    hal_atomic_set(&q->read_cursor, read + 1u);
    return true;
}
