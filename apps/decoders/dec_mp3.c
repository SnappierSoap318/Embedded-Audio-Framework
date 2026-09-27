#include <eaf/eaf_dec_mp3.h>

#include <dr_mp3.h>
#include <stdlib.h>

/* dr_mp3 asks for its data buffer in one read; only start once enough is
   buffered (or the stream is complete). With an await hook the read blocks.
   onSeek/onTell are deliberately NULL: dr_mp3 then reads past ID3v2 tags
   instead of seeking, which is the only option for a live stream. */
#define EAF_DEC_MP3_HEADER_MIN 4096u

static size_t mp3_read(void *ctx, void *out, size_t bytes) {
    return eaf_dec_input_read((eaf_dec_input_t *)ctx, out, bytes);
}

void eaf_dec_mp3_configure(eaf_dec_mp3_t *state, eaf_dec_await_fn await, void *await_ctx) {
    if (!state)
        return;
    eaf_dec_input_set_await(&state->input, await, await_ctx);
}

void eaf_dec_mp3_finish(eaf_dec_mp3_t *state) {
    if (state)
        eaf_dec_input_finish(&state->input);
}

static void mp3_uninit(eaf_dec_mp3_t *state) {
    if (state->mp3 != NULL && state->initialized) {
        drmp3_uninit((drmp3 *)state->mp3);
        state->initialized = false;
    }
}

static int mp3_open(void *ctx, const eaf_decoder_config_t *cfg, const eaf_format_t *output) {
    eaf_dec_mp3_t *state = ctx;
    int rc = eaf_decoder_validate_open(cfg, output, EAF_CODEC_MP3, true);
    if (rc)
        return rc;
    mp3_uninit(state);
    if (state->mp3 == NULL) {
        state->mp3 = malloc(sizeof(drmp3));
        if (state->mp3 == NULL)
            return EAF_IO;
    }
    state->initialized = false;
    eaf_dec_input_init(&state->input, state->ring, EAF_DEC_MP3_RING_BYTES, state->input.await,
                       state->input.await_ctx);
    state->cfg_channels = cfg->channels;
    state->output_channels = output->num_channels;
    state->format = *output;
    return EAF_OK;
}

static int mp3_push(void *ctx, const uint8_t *data, size_t length, size_t *consumed) {
    eaf_dec_mp3_t *state = ctx;
    return eaf_dec_input_push_adapter(&state->input, data, length, consumed);
}

static int mp3_pull(void *ctx, int32_t *pcm, uint32_t max_frames, uint32_t *frames,
                    eaf_format_t *format) {
    eaf_dec_mp3_t *state = ctx;
    *frames = 0;
    if (state->mp3 == NULL)
        return EAF_STATE;
    if (!state->initialized) {
        if (state->input.await == NULL && state->input.count < EAF_DEC_MP3_HEADER_MIN &&
            !state->input.eof)
            return EAF_OK; /* wait for the header/first frames */
        if (drmp3_init((drmp3 *)state->mp3, mp3_read, NULL, NULL, NULL, &state->input, NULL) ==
            DRMP3_FALSE) {
            eaf_dec_input_reset(&state->input);
            return EAF_INVALID;
        }
        state->initialized = true;
        drmp3 *decoder = (drmp3 *)state->mp3;
        if (decoder->channels == 0u || decoder->channels > 2u ||
            decoder->sampleRate != state->format.sample_rate) {
            mp3_uninit(state);
            eaf_dec_input_reset(&state->input);
            return EAF_UNSUPPORTED;
        }
        if (state->cfg_channels != 0u && state->cfg_channels != decoder->channels) {
            mp3_uninit(state);
            eaf_dec_input_reset(&state->input);
            return EAF_INVALID;
        }
        state->mp3_channels = (uint8_t)decoder->channels;
        if (state->output_channels != state->mp3_channels &&
            !(state->mp3_channels == 1u && state->output_channels == 2u)) {
            mp3_uninit(state);
            eaf_dec_input_reset(&state->input);
            return EAF_UNSUPPORTED;
        }
    }
    uint32_t cap = max_frames;
    if (cap > EAF_DEC_MP3_TEMP_FRAMES)
        cap = EAF_DEC_MP3_TEMP_FRAMES;
    if (cap == 0u)
        return EAF_INVALID;
    size_t got =
        (size_t)drmp3_read_pcm_frames_s16((drmp3 *)state->mp3, (drmp3_uint64)cap, state->temp);
    if (got == 0u)
        return EAF_OK;
    eaf_s16_to_q31_interleaved(state->temp, (uint32_t)got, state->mp3_channels, pcm,
                               state->output_channels);
    *frames = (uint32_t)got;
    *format = state->format;
    return EAF_OK;
}

static int mp3_reset(void *ctx) {
    eaf_dec_mp3_t *state = ctx;
    mp3_uninit(state);
    eaf_dec_input_reset(&state->input);
    return EAF_OK;
}

static void mp3_close(void *ctx) {
    eaf_dec_mp3_t *state = ctx;
    mp3_uninit(state);
    free(state->mp3);
    state->mp3 = NULL;
    eaf_dec_input_reset(&state->input);
}

const eaf_decoder_ops_t eaf_dec_mp3_ops = {
    .open = mp3_open, .push = mp3_push, .pull = mp3_pull, .reset = mp3_reset, .close = mp3_close};
