#include <eaf/eaf_dec_opus.h>

#include <opus.h>
#include <string.h>

static int opus_open(void *ctx, const eaf_decoder_config_t *cfg, const eaf_format_t *output) {
    eaf_dec_opus_t *state = ctx;
    if (cfg->codec != EAF_CODEC_OPUS)
        return EAF_UNSUPPORTED;
    if (cfg->channels == 0u || cfg->channels > 2u)
        return EAF_UNSUPPORTED;
    if (output->num_channels != cfg->channels &&
        !(cfg->channels == 1u && output->num_channels == 2u))
        return EAF_UNSUPPORTED;
    if (cfg->sample_rate != output->sample_rate)
        return EAF_UNSUPPORTED;
    int error = OPUS_OK;
    OpusDecoder *decoder =
        opus_decoder_create((opus_int32)cfg->sample_rate, (int)cfg->channels, &error);
    if (decoder == NULL || error != OPUS_OK)
        return EAF_UNSUPPORTED;
    state->dec = decoder;
    state->channels = cfg->channels;
    state->output_channels = output->num_channels;
    state->packet_length = 0;
    state->decoded_frames = 0;
    state->decoded_pos = 0;
    state->format = *output;
    return EAF_OK;
}

static int opus_push(void *ctx, const uint8_t *data, size_t length, size_t *consumed) {
    eaf_dec_opus_t *state = ctx;
    *consumed = 0;
    if (state->packet_length != 0u)
        return EAF_OK; /* one packet at a time: step before pushing again */
    if (length > EAF_DEC_OPUS_MAX_PACKET)
        return EAF_INVALID;
    memcpy(state->packet, data, length);
    state->packet_length = length;
    *consumed = length;
    return EAF_OK;
}

static int opus_pull(void *ctx, int32_t *pcm, uint32_t max_frames, uint32_t *frames,
                     eaf_format_t *format) {
    eaf_dec_opus_t *state = ctx;
    *frames = 0;
    if (max_frames == 0u)
        return EAF_INVALID;
    if (state->decoded_pos >= state->decoded_frames) {
        state->decoded_pos = 0;
        state->decoded_frames = 0;
        if (state->packet_length == 0u)
            return EAF_OK;
        int samples =
            opus_decode((OpusDecoder *)state->dec, state->packet, (opus_int32)state->packet_length,
                        state->temp, (int)EAF_DEC_OPUS_MAX_FRAMES, 0);
        state->packet_length = 0;
        if (samples < 0)
            return EAF_INVALID;
        state->decoded_frames = (uint32_t)samples;
        if (state->decoded_frames == 0u)
            return EAF_OK;
    }
    uint32_t available = state->decoded_frames - state->decoded_pos;
    uint32_t count = available < max_frames ? available : max_frames;
    for (uint32_t i = 0; i < count; ++i) {
        size_t source = (size_t)(state->decoded_pos + i) * state->channels;
        int32_t left = eaf_pcm16_to_q31(state->temp[source]);
        if (state->output_channels == 2u) {
            int32_t right =
                state->channels == 2u ? eaf_pcm16_to_q31(state->temp[source + 1u]) : left;
            pcm[(size_t)i * 2u] = left;
            pcm[(size_t)i * 2u + 1u] = right;
        } else {
            pcm[i] = left;
        }
    }
    state->decoded_pos += count;
    *frames = count;
    *format = state->format;
    return EAF_OK;
}

static int opus_reset(void *ctx) {
    eaf_dec_opus_t *state = ctx;
    state->packet_length = 0;
    state->decoded_frames = 0;
    state->decoded_pos = 0;
    if (state->dec)
        (void)opus_decoder_ctl((OpusDecoder *)state->dec, OPUS_RESET_STATE);
    return EAF_OK;
}

static void opus_close(void *ctx) {
    eaf_dec_opus_t *state = ctx;
    if (state->dec) {
        opus_decoder_destroy((OpusDecoder *)state->dec);
        state->dec = NULL;
    }
    state->packet_length = 0;
    state->decoded_frames = 0;
    state->decoded_pos = 0;
}

const eaf_decoder_ops_t eaf_dec_opus_ops = {.open = opus_open,
                                            .push = opus_push,
                                            .pull = opus_pull,
                                            .reset = opus_reset,
                                            .close = opus_close};
