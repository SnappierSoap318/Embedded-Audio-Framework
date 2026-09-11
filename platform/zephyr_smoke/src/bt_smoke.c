/* Exercise the adapter against the pinned Zephyr API without a radio. */
#include <eaf/eaf_bt_zephyr.h>
#include <errno.h>
#include <zephyr/bluetooth/classic/a2dp.h>
#include <zephyr/bluetooth/classic/a2dp_codec_sbc.h>

static struct bt_a2dp_cb *cb;
static struct bt_a2dp_ep *ep;
static struct bt_a2dp_stream *registered_stream;
static eaf_bt_ingress_t queue;
static eaf_bt_packet_t packet;
static unsigned events;
int bt_a2dp_register_cb(struct bt_a2dp_cb *callbacks) {
    cb = callbacks;
    return 0;
}
int bt_a2dp_register_ep(struct bt_a2dp_ep *endpoint, uint8_t media_type, uint8_t sep_type) {
    if (media_type != BT_AVDTP_AUDIO || sep_type != BT_AVDTP_SINK)
        return -EINVAL;
    ep = endpoint;
    return 0;
}
void bt_a2dp_stream_cb_register(struct bt_a2dp_stream *stream, struct bt_a2dp_stream_ops *ops) {
    registered_stream = stream;
    stream->ops = ops;
}
static void event(void *ctx, eaf_bt_zephyr_event_t value) {
    (void)ctx;
    events |= 1u << (unsigned)value;
}
int eaf_bt_smoke(void) {
    eaf_bt_ingress_init(&queue);
    if (eaf_bt_zephyr_register(&queue, 32000, event, NULL) != -EINVAL ||
        eaf_bt_zephyr_register(&queue, 48000, event, NULL) ||
        eaf_bt_zephyr_register(&queue, 48000, event, NULL) != -EALREADY)
        return -1;
    struct bt_a2dp_codec_ie ie = {.len = 4, .codec_ie = {0x11, 0x15, 2, 53}};
    struct bt_a2dp_codec_cfg cfg = {.codec_config = &ie};
    struct bt_a2dp_stream *stream = NULL;
    uint8_t error = 0;
    if (cb->config_req(NULL, ep, &cfg, &stream, &error) || stream != registered_stream)
        return -2;
    ie.codec_ie[0] = 0x31; /* Multiple sample rates are not a configuration. */
    if (!cb->reconfig_req(stream, &cfg, &error) ||
        error != BT_A2DP_NOT_SUPPORTED_SAMPLING_FREQUENCY)
        return -3;
    ie.codec_ie[0] = 0x13; /* Multiple channel modes. */
    if (!cb->reconfig_req(stream, &cfg, &error) || error != BT_A2DP_INVALID_CHANNEL_MODE)
        return -4;
    ie.codec_ie[0] = 0x11;
    ie.codec_ie[3] = 54;
    if (!cb->reconfig_req(stream, &cfg, &error) || error != BT_A2DP_INVALID_MAXIMUM_BITPOOL_VALUE)
        return -5;
    stream->ops->configured(stream);
    uint8_t media[] = {1, 0x9c, 0};
    struct net_buf buf = {0};
    net_buf_simple_init_with_data(&buf.b, media, sizeof(media));
    stream->ops->recv(stream, &buf, 1, 128);
    if (eaf_bt_ingress_pop(&queue, &packet))
        return -6;
    stream->ops->started(stream);
    stream->ops->recv(stream, &buf, 1, 128);
    if (!eaf_bt_ingress_pop(&queue, &packet) || !packet.discontinuity || packet.sequence != 1 ||
        packet.timestamp != 128 || packet.length != 2 || packet.data[0] != 0x9c)
        return -7;
    stream->ops->suspended(stream);
    stream->ops->recv(stream, &buf, 2, 256);
    if (eaf_bt_ingress_pop(&queue, &packet))
        return -8;
    stream->ops->started(stream);
    stream->ops->recv(stream, &buf, 3, 384);
    if (!eaf_bt_ingress_pop(&queue, &packet) || !packet.discontinuity)
        return -9;
    stream->ops->released(stream);
    stream->ops->recv(stream, &buf, 4, 512);
    if (eaf_bt_ingress_pop(&queue, &packet) || events != 15u)
        return -10;
    return 0;
}
