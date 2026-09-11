#include <eaf/eaf_bt_decoder.h>
int eaf_bt_decoder_init(eaf_bt_decoder_t *d, eaf_bt_ingress_t *q, eaf_reservoir_t *r,
                        const eaf_sbc_decoder_t *codec) {
    if (!d || !q || !r || !r->storage || !codec || !codec->frame || !codec->reset ||
        r->format.num_channels != 2 || r->format.channel_mask != 3)
        return EAF_INVALID;
    *d = (eaf_bt_decoder_t){.ingress = q, .reservoir = r, .decoder = *codec};
    return d->decoder.reset(d->decoder.ctx);
}
static int reject(eaf_bt_decoder_t *d, int rc) {
    ++d->rejected;
    d->pending = 0;
    d->sent = 0;
    d->frames_left = 0;
    int reset = d->decoder.reset(d->decoder.ctx);
    return reset ? reset : rc;
}
int eaf_bt_decoder_step(eaf_bt_decoder_t *d) {
    if (!d || !d->ingress || !d->reservoir)
        return EAF_INVALID;
    if (d->pending > d->sent) {
        d->sent += eaf_reservoir_write(d->reservoir, d->pcm31 + (size_t)d->sent * 2u,
                                       d->pending - d->sent);
        if (d->pending > d->sent)
            return EAF_OK;
    }
    d->pending = 0;
    d->sent = 0;
    if (!d->frames_left) {
        if (!eaf_bt_ingress_pop(d->ingress, &d->packet))
            return EAF_OK;
        d->offset = 0;
        d->frames_left = d->packet.frame_count;
        if (d->packet.discontinuity) {
            int rc = d->decoder.reset(d->decoder.ctx);
            if (rc)
                return reject(d, rc);
        }
    }
    size_t used = 0;
    uint32_t frames = 0;
    eaf_format_t format = {0};
    int rc = d->decoder.frame(d->decoder.ctx, d->packet.data + d->offset,
                              d->packet.length - d->offset, &used, d->pcm16, &frames, &format);
    if (rc)
        return reject(d, rc);
    if (!used || used > d->packet.length - d->offset || !frames || frames > 128 ||
        !eaf_format_equal(&format, &d->reservoir->format))
        return reject(d, EAF_INVALID);
    d->offset += used;
    --d->frames_left;
    if ((!d->frames_left && d->offset != d->packet.length) ||
        (d->frames_left && d->offset == d->packet.length))
        return reject(d, EAF_INVALID);
    for (size_t i = 0; i < (size_t)frames * 2u; ++i)
        d->pcm31[i] = eaf_pcm16_to_q31(d->pcm16[i]);
    d->pending = frames;
    d->sent = eaf_reservoir_write(d->reservoir, d->pcm31, frames);
    return EAF_OK;
}
