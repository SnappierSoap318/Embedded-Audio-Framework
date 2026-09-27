#include <eaf/eaf_dec_pcm.h>

static int pcm_open(void *ctx, const eaf_decoder_config_t *cfg, const eaf_format_t *output) {
    eaf_decoder_pcm_t *state = ctx;
    int rc = eaf_decoder_validate_open(cfg, output, EAF_CODEC_PCM, false);
    if (rc)
        return rc;
    if (cfg->bit_depth != 16u && cfg->bit_depth != 24u && cfg->bit_depth != 32u)
        return EAF_UNSUPPORTED;
    state->input_channels = cfg->channels;
    state->input_bits = cfg->bit_depth;
    state->output_channels = output->num_channels;
    state->sample_bytes = (size_t)cfg->bit_depth / 8u;
    state->frame_bytes = (size_t)cfg->channels * state->sample_bytes;
    state->format = *output;
    eaf_dec_input_init(&state->input, state->ring, EAF_DECODER_PCM_RING_BYTES, NULL, NULL);
    return EAF_OK;
}

static int pcm_push(void *ctx, const uint8_t *data, size_t length, size_t *consumed) {
    eaf_decoder_pcm_t *state = ctx;
    *consumed = eaf_dec_input_push(&state->input, data, length);
    return EAF_OK;
}

static int32_t pcm_sample(const eaf_decoder_pcm_t *state, const uint8_t *frame, size_t offset) {
    uint32_t value = 0;
    for (size_t i = 0; i < state->sample_bytes; ++i)
        value |= (uint32_t)frame[offset + i] << (8u * i);
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
    size_t available = state->input.count / state->frame_bytes;
    size_t count = available < max_frames ? available : (size_t)max_frames;
    for (size_t f = 0; f < count; ++f) {
        uint8_t frame[8];
        int32_t converted[2] = {0, 0};
        (void)eaf_dec_input_read(&state->input, frame, state->frame_bytes);
        converted[0] = pcm_sample(state, frame, 0);
        if (state->input_channels == 2u)
            converted[1] = pcm_sample(state, frame, state->sample_bytes);
        eaf_q31_interleaved(converted, 1u, state->input_channels, pcm + f * state->output_channels,
                            state->output_channels);
    }
    *frames = (uint32_t)count;
    *format = state->format;
    return EAF_OK;
}

static int pcm_reset(void *ctx) {
    eaf_decoder_pcm_t *state = ctx;
    eaf_dec_input_reset(&state->input);
    return EAF_OK;
}

static void pcm_close(void *ctx) {
    (void)ctx;
}

const eaf_decoder_ops_t eaf_decoder_pcm_ops = {
    .open = pcm_open, .push = pcm_push, .pull = pcm_pull, .reset = pcm_reset, .close = pcm_close};
