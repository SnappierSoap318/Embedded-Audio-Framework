#include <eaf/eaf_dec_vorbis.h>

#include <stb_vorbis_header.h>

#include <string.h>

static void vorbis_release(eaf_dec_vorbis_t *state) {
    if (state->v) {
        stb_vorbis_close((stb_vorbis *)state->v);
        state->v = NULL;
    }
}

static int vorbis_open(void *ctx, const eaf_decoder_config_t *cfg, const eaf_format_t *output) {
    eaf_dec_vorbis_t *state = ctx;
    int rc = eaf_decoder_validate_open(cfg, output, EAF_CODEC_VORBIS, true);
    if (rc)
        return rc;
    vorbis_release(state);
    state->length = 0;
    state->vorbis_channels = cfg->channels;
    state->output_channels = output->num_channels;
    state->format = *output;
    return EAF_OK;
}

static int vorbis_push(void *ctx, const uint8_t *data, size_t length, size_t *consumed) {
    eaf_dec_vorbis_t *state = ctx;
    size_t space = EAF_DEC_VORBIS_BUFFER_BYTES - state->length;
    size_t take = length < space ? length : space;
    memcpy(state->buffer + state->length, data, take);
    state->length += take;
    *consumed = take;
    return EAF_OK;
}

static int vorbis_pull(void *ctx, int32_t *pcm, uint32_t max_frames, uint32_t *frames,
                       eaf_format_t *format) {
    eaf_dec_vorbis_t *state = ctx;
    *frames = 0;
    if (state->v == NULL) {
        if (state->length == 0u)
            return EAF_OK;
        int consumed = 0;
        int error = 0;
        stb_vorbis *decoder =
            stb_vorbis_open_pushdata(state->buffer, (int)state->length, &consumed, &error, NULL);
        if (decoder == NULL) {
            if (error == VORBIS_need_more_data)
                return EAF_OK;
            state->length = 0;
            return EAF_INVALID;
        }
        state->v = decoder;
        memmove(state->buffer, state->buffer + consumed, state->length - (size_t)consumed);
        state->length -= (size_t)consumed;
    }
    for (;;) {
        if (state->length == 0u)
            return EAF_OK;
        int channels = 0;
        int samples = 0;
        float **output = NULL;
        int consumed =
            stb_vorbis_decode_frame_pushdata((stb_vorbis *)state->v, state->buffer,
                                             (int)state->length, &channels, &output, &samples);
        if (consumed > 0) {
            memmove(state->buffer, state->buffer + consumed, state->length - (size_t)consumed);
            state->length -= (size_t)consumed;
        }
        if (samples > 0) {
            if (channels < 1 || channels > 2)
                return EAF_UNSUPPORTED;
            if (state->vorbis_channels == 0u)
                state->vorbis_channels = (uint8_t)channels;
            if (state->vorbis_channels != (uint8_t)channels)
                return EAF_INVALID;
            if (state->output_channels != (uint8_t)channels &&
                !(channels == 1 && state->output_channels == 2u))
                return EAF_UNSUPPORTED;
            if ((uint32_t)samples > max_frames)
                return EAF_INVALID; /* owner must supply a full-frame buffer */
            for (uint32_t i = 0; i < (uint32_t)samples; ++i) {
                int32_t left = eaf_float_to_q31(output[0][i]);
                if (state->output_channels == 2u) {
                    int32_t right = channels > 1 ? eaf_float_to_q31(output[1][i]) : left;
                    pcm[(size_t)i * 2u] = left;
                    pcm[(size_t)i * 2u + 1u] = right;
                } else {
                    pcm[i] = left;
                }
            }
            *frames = (uint32_t)samples;
            *format = state->format;
            return EAF_OK;
        }
        if (consumed == 0)
            return EAF_OK;
    }
}

static int vorbis_reset(void *ctx) {
    eaf_dec_vorbis_t *state = ctx;
    vorbis_release(state);
    state->length = 0;
    state->vorbis_channels = 0;
    return EAF_OK;
}

static void vorbis_close(void *ctx) {
    eaf_dec_vorbis_t *state = ctx;
    vorbis_release(state);
    state->length = 0;
}

const eaf_decoder_ops_t eaf_dec_vorbis_ops = {.open = vorbis_open,
                                              .push = vorbis_push,
                                              .pull = vorbis_pull,
                                              .reset = vorbis_reset,
                                              .close = vorbis_close};
