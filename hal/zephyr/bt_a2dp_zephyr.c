#include <eaf/eaf_bt_zephyr.h>
#include <errno.h>
#include <zephyr/bluetooth/classic/a2dp.h>
#include <zephyr/bluetooth/classic/a2dp_codec_sbc.h>

static eaf_bt_ingress_t *ingress;
static eaf_bt_zephyr_notify_t notify_owner;
static void *owner_ctx;
static bool attempted, streaming;
static struct bt_a2dp_stream sink_stream;
static struct bt_a2dp_codec_ie capability = {
    .len = BT_A2DP_SBC_IE_LENGTH,
    .codec_ie = {A2DP_SBC_SAMP_FREQ_48000 | A2DP_SBC_CH_MODE_STEREO | A2DP_SBC_CH_MODE_JOINT,
                 A2DP_SBC_BLK_LEN_16 | A2DP_SBC_SUBBAND_8 | A2DP_SBC_ALLOC_MTHD_LOUDNESS, 2, 53}};
static struct bt_a2dp_ep endpoint = BT_A2DP_SINK_EP_INIT(BT_A2DP_SBC, &capability, false);

static int validate(const struct bt_a2dp_codec_cfg *cfg, uint8_t *error) {
    if (!cfg || !cfg->codec_config || cfg->codec_config->len != BT_A2DP_SBC_IE_LENGTH) {
        *error = BT_A2DP_INVALID_CODEC_TYPE;
        return -EINVAL;
    }
    const uint8_t *ie = cfg->codec_config->codec_ie;
    if ((ie[0] & 0xf0u) != (capability.codec_ie[0] & 0xf0u))
        *error = BT_A2DP_NOT_SUPPORTED_SAMPLING_FREQUENCY;
    else if ((ie[0] & 0x0fu) != A2DP_SBC_CH_MODE_STEREO &&
             (ie[0] & 0x0fu) != A2DP_SBC_CH_MODE_JOINT)
        *error = BT_A2DP_INVALID_CHANNEL_MODE;
    else if ((ie[1] & 0xf0u) != A2DP_SBC_BLK_LEN_16)
        *error = BT_A2DP_INVALID_BLOCK_LENGTH;
    else if ((ie[1] & 0x0cu) != A2DP_SBC_SUBBAND_8)
        *error = BT_A2DP_INVALID_SUBBANDS;
    else if ((ie[1] & 0x03u) != A2DP_SBC_ALLOC_MTHD_LOUDNESS)
        *error = BT_A2DP_INVALID_ALLOCATION_METHOD;
    else if (ie[2] < 2 || ie[2] > ie[3])
        *error = BT_A2DP_INVALID_MINIMUM_BITPOOL_VALUE;
    else if (ie[3] > 53)
        *error = BT_A2DP_INVALID_MAXIMUM_BITPOOL_VALUE;
    else {
        *error = 0;
        return 0;
    }
    return -EINVAL;
}
static int configure(struct bt_a2dp *a2dp, struct bt_a2dp_ep *ep, struct bt_a2dp_codec_cfg *cfg,
                     struct bt_a2dp_stream **stream, uint8_t *error) {
    (void)a2dp;
    if (ep != &endpoint || sink_stream.a2dp) {
        *error = BT_AVDTP_BAD_STATE;
        return -EBUSY;
    }
    int result = validate(cfg, error);
    if (!result)
        *stream = &sink_stream;
    return result;
}
static int reconfigure(struct bt_a2dp_stream *stream, struct bt_a2dp_codec_cfg *cfg,
                       uint8_t *error) {
    (void)stream;
    return validate(cfg, error);
}
static void configured(struct bt_a2dp_stream *stream) {
    (void)stream;
    streaming = false;
    ingress->discontinuity = true;
    notify_owner(owner_ctx, EAF_BT_CONFIGURED);
}
static void started(struct bt_a2dp_stream *stream) {
    (void)stream;
    ingress->discontinuity = true;
    streaming = true;
    notify_owner(owner_ctx, EAF_BT_STARTED);
}
static void suspended(struct bt_a2dp_stream *stream) {
    (void)stream;
    streaming = false;
    ingress->discontinuity = true;
    notify_owner(owner_ctx, EAF_BT_SUSPENDED);
}
static void released(struct bt_a2dp_stream *stream) {
    (void)stream;
    streaming = false;
    ingress->discontinuity = true;
    notify_owner(owner_ctx, EAF_BT_RELEASED);
}
static void receive(struct bt_a2dp_stream *stream, struct net_buf *buf, uint16_t sequence,
                    uint32_t timestamp) {
    (void)stream;
    if (!streaming)
        return;
    /* Zephyr removes the fixed RTP header before recv. Do not retain or unref
     * its buffer. Chained buffers cannot be passed as contiguous media. */
    if (buf->frags) {
        (void)eaf_bt_sbc_receive(ingress, sequence, timestamp, NULL, 0);
        return;
    }
    (void)eaf_bt_sbc_receive(ingress, sequence, timestamp, buf->data, buf->len);
}
static struct bt_a2dp_stream_ops stream_ops = {.configured = configured,
                                               .started = started,
                                               .suspended = suspended,
                                               .released = released,
                                               .recv = receive};
static struct bt_a2dp_cb callbacks = {.config_req = configure, .reconfig_req = reconfigure};
int eaf_bt_zephyr_register(eaf_bt_ingress_t *queue, uint32_t rate, eaf_bt_zephyr_notify_t notify,
                           void *ctx) {
    if (!queue || !notify || (rate != 44100 && rate != 48000))
        return -EINVAL;
    if (attempted)
        return -EALREADY;
    attempted = true;
    ingress = queue;
    notify_owner = notify;
    owner_ctx = ctx;
    capability.codec_ie[0] =
        (uint8_t)((rate == 44100 ? A2DP_SBC_SAMP_FREQ_44100 : A2DP_SBC_SAMP_FREQ_48000) |
                  A2DP_SBC_CH_MODE_STEREO | A2DP_SBC_CH_MODE_JOINT);
    bt_a2dp_stream_cb_register(&sink_stream, &stream_ops);
    int result = bt_a2dp_register_cb(&callbacks);
    if (result)
        return result;
    return bt_a2dp_register_ep(&endpoint, BT_AVDTP_AUDIO, BT_AVDTP_SINK);
}
