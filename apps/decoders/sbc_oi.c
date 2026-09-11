#include <eaf/eaf_sbc_oi.h>
#include <string.h>
static int reset(void *ctx) {
    eaf_sbc_oi_t *s = ctx;
    memset(s, 0, sizeof(*s));
    OI_STATUS rc = OI_CODEC_SBC_DecoderReset(&s->context, s->data, sizeof(s->data), 2, 2, FALSE);
    return OI_SUCCESS(rc) ? EAF_OK : EAF_IO;
}
static int frame(void *ctx, const uint8_t *data, size_t length, size_t *consumed, int16_t pcm[256],
                 uint32_t *frames, eaf_format_t *fmt) {
    if (!ctx || !data || length < 4 || length > UINT32_MAX || !consumed || !pcm || !frames || !fmt)
        return EAF_INVALID;
    *consumed = 0;
    *frames = 0;
    if (data[0] != 0x9c)
        return EAF_INVALID; /* No scanning over corrupt bytes. */
    eaf_sbc_oi_t *s = ctx;
    uint32_t left = (uint32_t)length, bytes = 256u * sizeof(int16_t);
    const OI_BYTE *p = data;
    OI_STATUS rc = OI_CODEC_SBC_DecodeFrame(&s->context, &p, &left, pcm, &bytes);
    if (!OI_SUCCESS(rc))
        return EAF_INVALID;
    *consumed = length - left;
    *frames = bytes / (2u * sizeof(int16_t));
    *fmt = (eaf_format_t){s->context.common.frameInfo.frequency, 2, 3};
    return EAF_OK;
}
int eaf_sbc_oi_init(eaf_sbc_oi_t *state, eaf_sbc_decoder_t *decoder) {
    if (!state || !decoder)
        return EAF_INVALID;
    *decoder = (eaf_sbc_decoder_t){reset, frame, state};
    return reset(state);
}
