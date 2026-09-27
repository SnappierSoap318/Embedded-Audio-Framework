#include <eaf/eaf_dec_flac.h>

#include <dr_flac.h>
#include <string.h>

/* dr_flac asks for its L2 buffer (4 KB) in one read and latches EOF on a short
   count, so only start decoding once at least that much is buffered (or the
   stream is complete). With an await hook the read blocks instead. */
#define EAF_DEC_FLAC_HEADER_MIN 4096u

static size_t flac_read(void *ctx, void *out, size_t bytes) {
    return eaf_dec_input_read((eaf_dec_input_t *)ctx, out, bytes);
}

static drflac_bool32 flac_seek(void *ctx, int offset, drflac_seek_origin origin) {
    (void)ctx;
    (void)offset;
    (void)origin;
    return DRFLAC_FALSE;
}

void eaf_dec_flac_configure(eaf_dec_flac_t *state, eaf_dec_await_fn await, void *await_ctx) {
    if (!state)
        return;
    eaf_dec_input_set_await(&state->input, await, await_ctx);
}

void eaf_dec_flac_finish(eaf_dec_flac_t *state) {
    if (state)
        eaf_dec_input_finish(&state->input);
}

static int flac_open(void *ctx, const eaf_decoder_config_t *cfg, const eaf_format_t *output) {
    eaf_dec_flac_t *state = ctx;
    int rc = eaf_decoder_validate_open(cfg, output, EAF_CODEC_FLAC, false);
    if (rc)
        return rc;
    state->flac = NULL;
    eaf_dec_input_init(&state->input, state->ring, EAF_DEC_FLAC_RING_BYTES, state->input.await,
                       state->input.await_ctx);
    state->flac_channels = cfg->channels;
    state->output_channels = output->num_channels;
    state->format = *output;
    return EAF_OK;
}

static int flac_push(void *ctx, const uint8_t *data, size_t length, size_t *consumed) {
    eaf_dec_flac_t *state = ctx;
    return eaf_dec_input_push_adapter(&state->input, data, length, consumed);
}

static int flac_pull(void *ctx, int32_t *pcm, uint32_t max_frames, uint32_t *frames,
                     eaf_format_t *format) {
    eaf_dec_flac_t *state = ctx;
    *frames = 0;
    if (state->flac == NULL) {
        if (state->input.await == NULL && state->input.count < EAF_DEC_FLAC_HEADER_MIN &&
            !state->input.eof)
            return EAF_OK; /* wait for the header/metadata */
        state->flac = drflac_open(flac_read, flac_seek, NULL, &state->input, NULL);
        if (state->flac == NULL) {
            eaf_dec_input_reset(&state->input);
            return EAF_INVALID;
        }
    }
    uint32_t cap = max_frames;
    int32_t *dest = pcm;
    if (state->flac_channels != state->output_channels) {
        if (cap > EAF_DEC_FLAC_TEMP_FRAMES)
            cap = EAF_DEC_FLAC_TEMP_FRAMES;
        dest = state->temp;
    }
    if (cap == 0u)
        return EAF_INVALID;
    size_t got =
        (size_t)drflac_read_pcm_frames_s32((drflac *)state->flac, (drflac_uint64)cap, dest);
    if (got == 0u)
        return EAF_OK;
    if (dest == state->temp)
        eaf_q31_interleaved(state->temp, (uint32_t)got, state->flac_channels, pcm,
                            state->output_channels);
    *frames = (uint32_t)got;
    *format = state->format;
    return EAF_OK;
}

static void flac_release(eaf_dec_flac_t *state) {
    if (state->flac) {
        drflac_close((drflac *)state->flac);
        state->flac = NULL;
    }
}

static int flac_reset(void *ctx) {
    eaf_dec_flac_t *state = ctx;
    flac_release(state);
    eaf_dec_input_reset(&state->input);
    return EAF_OK;
}

static void flac_close(void *ctx) {
    eaf_dec_flac_t *state = ctx;
    flac_release(state);
    eaf_dec_input_reset(&state->input);
}

const eaf_decoder_ops_t eaf_dec_flac_ops = {.open = flac_open,
                                            .push = flac_push,
                                            .pull = flac_pull,
                                            .reset = flac_reset,
                                            .close = flac_close};
