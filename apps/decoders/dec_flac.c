#include <eaf/eaf_dec_flac.h>

#include <dr_flac.h>
#include <string.h>

/* dr_flac asks for its L2 buffer (4 KB) in one read and latches EOF on a short
   count, so only start decoding once at least that much is buffered (or the
   stream is complete). With an await hook the read blocks instead. */
#define EAF_DEC_FLAC_HEADER_MIN 4096u

static size_t flac_read(void *ctx, void *out, size_t bytes) {
    eaf_dec_flac_t *state = ctx;
    uint8_t *dst = out;
    size_t got = 0;
    while (got < bytes) {
        if (state->count == 0u) {
            if (state->eof || state->await == NULL)
                break;
            if (state->await(state->await_ctx) != 0)
                break;
            continue;
        }
        size_t chunk = state->capacity - state->tail;
        if (chunk > state->count)
            chunk = state->count;
        if (chunk > bytes - got)
            chunk = bytes - got;
        memcpy(dst + got, state->ring + state->tail, chunk);
        state->tail = (state->tail + chunk) % state->capacity;
        state->count -= chunk;
        got += chunk;
    }
    return got;
}

static drflac_bool32 flac_seek(void *ctx, int offset, drflac_seek_origin origin) {
    (void)ctx;
    (void)offset;
    (void)origin;
    return DRFLAC_FALSE;
}

void eaf_dec_flac_configure(eaf_dec_flac_t *state, eaf_dec_flac_await_fn await, void *await_ctx) {
    if (!state)
        return;
    state->await = await;
    state->await_ctx = await_ctx;
}

void eaf_dec_flac_finish(eaf_dec_flac_t *state) {
    if (state)
        state->eof = true;
}

static int flac_open(void *ctx, const eaf_decoder_config_t *cfg, const eaf_format_t *output) {
    eaf_dec_flac_t *state = ctx;
    if (cfg->codec != EAF_CODEC_FLAC)
        return EAF_UNSUPPORTED;
    if (cfg->channels == 0u || cfg->channels > 2u)
        return EAF_UNSUPPORTED;
    if (output->num_channels != cfg->channels &&
        !(cfg->channels == 1u && output->num_channels == 2u))
        return EAF_UNSUPPORTED;
    if (cfg->sample_rate != output->sample_rate)
        return EAF_UNSUPPORTED;
    state->flac = NULL;
    state->capacity = EAF_DEC_FLAC_RING_BYTES;
    state->tail = 0;
    state->count = 0;
    state->eof = false;
    state->flac_channels = cfg->channels;
    state->output_channels = output->num_channels;
    state->format = *output;
    return EAF_OK;
}

static int flac_push(void *ctx, const uint8_t *data, size_t length, size_t *consumed) {
    eaf_dec_flac_t *state = ctx;
    *consumed = 0;
    if (state->eof)
        return EAF_OK;
    size_t space = state->capacity - state->count;
    size_t take = length < space ? length : space;
    size_t write = (state->tail + state->count) % state->capacity;
    for (size_t i = 0; i < take; ++i) {
        state->ring[write] = data[i];
        write = (write + 1u) % state->capacity;
    }
    state->count += take;
    *consumed = take;
    return EAF_OK;
}

static int flac_pull(void *ctx, int32_t *pcm, uint32_t max_frames, uint32_t *frames,
                     eaf_format_t *format) {
    eaf_dec_flac_t *state = ctx;
    *frames = 0;
    if (state->flac == NULL) {
        if (state->await == NULL && state->count < EAF_DEC_FLAC_HEADER_MIN && !state->eof)
            return EAF_OK; /* wait for the header/metadata */
        state->flac = drflac_open(flac_read, flac_seek, NULL, state, NULL);
        if (state->flac == NULL) {
            state->tail = 0;
            state->count = 0;
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
    if (dest == state->temp) {
        for (size_t i = 0; i < got; ++i) {
            int32_t value = state->temp[i];
            pcm[i * 2u] = value;
            pcm[i * 2u + 1u] = value;
        }
    }
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
    state->tail = 0;
    state->count = 0;
    state->eof = false;
    return EAF_OK;
}

static void flac_close(void *ctx) {
    eaf_dec_flac_t *state = ctx;
    flac_release(state);
    state->tail = 0;
    state->count = 0;
    state->eof = false;
}

const eaf_decoder_ops_t eaf_dec_flac_ops = {.open = flac_open,
                                            .push = flac_push,
                                            .pull = flac_pull,
                                            .reset = flac_reset,
                                            .close = flac_close};
