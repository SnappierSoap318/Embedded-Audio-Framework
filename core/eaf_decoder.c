#include <eaf/eaf_decoder.h>

int eaf_decoder_open(eaf_decoder_t *decoder, const eaf_decoder_config_t *cfg,
                     const eaf_format_t *output) {
    if (!decoder || !decoder->ops || !decoder->ctx || !cfg || !output)
        return EAF_INVALID;
    if (!decoder->ops->open || !decoder->ops->push || !decoder->ops->pull || !decoder->ops->reset ||
        !decoder->ops->close)
        return EAF_INVALID;
    if (!eaf_format_valid(output) || cfg->sample_rate == 0u || cfg->channels == 0u ||
        cfg->channels > EAF_MAX_CHANNELS)
        return EAF_INVALID;
    return decoder->ops->open(decoder->ctx, cfg, output);
}

int eaf_decoder_push(eaf_decoder_t *decoder, const uint8_t *data, size_t length, size_t *consumed) {
    if (!decoder || !decoder->ops || !decoder->ops->push || !consumed)
        return EAF_INVALID;
    *consumed = 0;
    if (length != 0u && !data)
        return EAF_INVALID;
    return decoder->ops->push(decoder->ctx, data, length, consumed);
}

int eaf_decoder_pull(eaf_decoder_t *decoder, int32_t *pcm, uint32_t max_frames, uint32_t *frames,
                     eaf_format_t *format) {
    if (!decoder || !decoder->ops || !decoder->ops->pull || !pcm || !frames || !format ||
        max_frames == 0u)
        return EAF_INVALID;
    *frames = 0;
    return decoder->ops->pull(decoder->ctx, pcm, max_frames, frames, format);
}

int eaf_decoder_reset(eaf_decoder_t *decoder) {
    if (!decoder || !decoder->ops || !decoder->ops->reset)
        return EAF_INVALID;
    return decoder->ops->reset(decoder->ctx);
}

void eaf_decoder_close(eaf_decoder_t *decoder) {
    if (decoder && decoder->ops && decoder->ops->close)
        decoder->ops->close(decoder->ctx);
}

int eaf_decode_worker_init(eaf_decode_worker_t *worker, const eaf_decoder_t *decoder,
                           eaf_pcm_write_fn sink, void *sink_ctx, int32_t *scratch,
                           uint32_t scratch_frames, const eaf_format_t *output) {
    if (!worker || !decoder || !decoder->ops || !sink || !scratch || !output)
        return EAF_INVALID;
    if (!eaf_format_valid(output) || scratch_frames == 0u)
        return EAF_INVALID;
    *worker = (eaf_decode_worker_t){.decoder = *decoder,
                                    .format = *output,
                                    .sink = sink,
                                    .sink_ctx = sink_ctx,
                                    .scratch = scratch,
                                    .scratch_frames = scratch_frames};
    return EAF_OK;
}

int eaf_decode_worker_open(eaf_decode_worker_t *worker, const eaf_decoder_config_t *cfg) {
    if (!worker || !cfg || !worker->sink || !worker->scratch)
        return EAF_INVALID;
    int rc = eaf_decoder_open(&worker->decoder, cfg, &worker->format);
    if (rc)
        return rc;
    worker->pending = 0;
    worker->sent = 0;
    worker->frames_written = 0;
    worker->frames_dropped = 0;
    worker->open = true;
    return EAF_OK;
}

bool eaf_decode_worker_can_push(const eaf_decode_worker_t *worker) {
    return worker && worker->open && worker->pending == worker->sent;
}

int eaf_decode_worker_push(eaf_decode_worker_t *worker, const uint8_t *data, size_t length,
                           size_t *consumed) {
    if (!worker || !consumed)
        return EAF_INVALID;
    *consumed = 0;
    if (!worker->open)
        return EAF_STATE;
    if (worker->pending > worker->sent)
        return EAF_OK; /* sink backpressure: step before feeding more input */
    return eaf_decoder_push(&worker->decoder, data, length, consumed);
}

static uint32_t drain(eaf_decode_worker_t *worker) {
    size_t channels = worker->format.num_channels;
    uint32_t written =
        worker->sink(worker->sink_ctx, worker->scratch + (size_t)worker->sent * channels,
                     worker->pending - worker->sent);
    worker->sent += written;
    worker->frames_written += written;
    return written;
}

int eaf_decode_worker_step(eaf_decode_worker_t *worker) {
    if (!worker)
        return EAF_INVALID;
    if (!worker->open)
        return EAF_STATE;
    if (worker->pending > worker->sent) {
        (void)drain(worker);
        return EAF_OK;
    }
    worker->pending = 0;
    worker->sent = 0;
    uint32_t frames = 0;
    eaf_format_t format = {0};
    int rc = eaf_decoder_pull(&worker->decoder, worker->scratch, worker->scratch_frames, &frames,
                              &format);
    if (rc) {
        (void)eaf_decoder_reset(&worker->decoder);
        return rc;
    }
    if (frames == 0u)
        return EAF_OK;
    if (!eaf_format_equal(&format, &worker->format)) {
        (void)eaf_decoder_reset(&worker->decoder);
        return EAF_INVALID;
    }
    worker->pending = frames;
    worker->sent = 0;
    (void)drain(worker);
    return EAF_OK;
}

int eaf_decode_worker_reset(eaf_decode_worker_t *worker) {
    if (!worker)
        return EAF_INVALID;
    if (worker->pending > worker->sent)
        worker->frames_dropped += worker->pending - worker->sent;
    worker->pending = 0;
    worker->sent = 0;
    if (!worker->open)
        return EAF_OK;
    return eaf_decoder_reset(&worker->decoder);
}

void eaf_decode_worker_close(eaf_decode_worker_t *worker) {
    if (!worker)
        return;
    if (worker->open)
        eaf_decoder_close(&worker->decoder);
    worker->open = false;
    worker->pending = 0;
    worker->sent = 0;
}
