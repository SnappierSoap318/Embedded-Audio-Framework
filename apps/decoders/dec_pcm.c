#include <eaf/eaf_dec_pcm.h>

static int pcm_open(void *ctx, const eaf_decoder_config_t *cfg, const eaf_format_t *output) {
    eaf_decoder_pcm_t *state = ctx;
    if (cfg->codec != EAF_CODEC_PCM)
        return EAF_UNSUPPORTED;
    if (cfg->bit_depth != 16u && cfg->bit_depth != 24u && cfg->bit_depth != 32u)
        return EAF_UNSUPPORTED;
    if (cfg->channels == 0u || cfg->channels > 2u)
        return EAF_UNSUPPORTED;
    if (output->num_channels != cfg->channels &&
        !(cfg->channels == 1u && output->num_channels == 2u))
        return EAF_UNSUPPORTED;
    if (cfg->sample_rate != output->sample_rate)
        return EAF_UNSUPPORTED;
    state->input_channels = cfg->channels;
    state->input_bits = cfg->bit_depth;
    state->output_channels = output->num_channels;
    state->sample_bytes = (size_t)cfg->bit_depth / 8u;
    state->frame_bytes = (size_t)cfg->channels * state->sample_bytes;
    state->format = *output;
    state->tail = 0;
    state->count = 0;
    return EAF_OK;
}

static int pcm_push(void *ctx, const uint8_t *data, size_t length, size_t *consumed) {
    eaf_decoder_pcm_t *state = ctx;
    size_t space = EAF_DECODER_PCM_RING_BYTES - state->count;
    size_t take = length < space ? length : space;
    for (size_t i = 0; i < take; ++i)
        state->ring[(state->tail + state->count + i) % EAF_DECODER_PCM_RING_BYTES] = data[i];
    state->count += take;
    *consumed = take;
    return EAF_OK;
}

static uint8_t ring_byte(const eaf_decoder_pcm_t *state, size_t offset) {
    return state->ring[(state->tail + offset) % EAF_DECODER_PCM_RING_BYTES];
}

static int32_t pcm_sample(const eaf_decoder_pcm_t *state, size_t offset) {
    uint32_t value = 0;
    for (size_t i = 0; i < state->sample_bytes; ++i)
        value |= (uint32_t)ring_byte(state, offset + i) << (8u * i);
    if (state->sample_bytes == 2u)
        return eaf_pcm16_to_q31((int16_t)value);
    if (state->sample_bytes == 3u) {
        if ((value & 0x800000u) != 0u)
            value |= 0xFF000000u;
        return eaf_pcm24_to_q31((int32_t)value);
    }
    return eaf_pcm32_to_q31((int32_t)value);
}

static int pcm_pull(void *ctx, int32_t *pcm, uint32_t max_frames, uint32_t *frames,
                    eaf_format_t *format) {
    eaf_decoder_pcm_t *state = ctx;
    size_t available = state->count / state->frame_bytes;
    size_t count = available < max_frames ? available : (size_t)max_frames;
    for (size_t f = 0; f < count; ++f) {
        size_t base = f * state->frame_bytes;
        int32_t left = pcm_sample(state, base);
        int32_t right =
            state->input_channels == 2u ? pcm_sample(state, base + state->sample_bytes) : left;
        int32_t *out = pcm + f * state->output_channels;
        out[0] = left;
        if (state->output_channels == 2u)
            out[1] = right;
    }
    size_t used = count * state->frame_bytes;
    state->tail = (state->tail + used) % EAF_DECODER_PCM_RING_BYTES;
    state->count -= used;
    *frames = (uint32_t)count;
    *format = state->format;
    return EAF_OK;
}

static int pcm_reset(void *ctx) {
    eaf_decoder_pcm_t *state = ctx;
    state->tail = 0;
    state->count = 0;
    return EAF_OK;
}

static void pcm_close(void *ctx) {
    (void)ctx;
}

const eaf_decoder_ops_t eaf_decoder_pcm_ops = {
    .open = pcm_open, .push = pcm_push, .pull = pcm_pull, .reset = pcm_reset, .close = pcm_close};
