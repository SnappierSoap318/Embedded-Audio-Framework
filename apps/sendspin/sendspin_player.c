#include <eaf/eaf_hal.h>
#include <eaf/eaf_sendspin_player.h>

/* Bounds the push/drain loops so a stalled sink can never spin the audio path. */
#define EAF_SENDPIN_DECODE_STEPS 64u

static int16_t read_le16(const uint8_t *p) {
    uint16_t value = (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
    return (int16_t)value;
}

static int32_t read_le24(const uint8_t *p) {
    uint32_t value = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    if (value & 0x800000u)
        value |= 0xFF000000u;
    return (int32_t)value;
}

static int32_t read_le32(const uint8_t *p) {
    uint32_t value =
        (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (int32_t)value;
}

static int32_t sample_to_q31(const uint8_t *p, uint8_t bits) {
    if (bits == 16u)
        return eaf_pcm16_to_q31(read_le16(p));
    if (bits == 24u)
        return eaf_pcm24_to_q31(read_le24(p));
    return eaf_pcm32_to_q31(read_le32(p));
}

static eaf_codec_t decoder_codec(eaf_sendspin_codec_t codec) {
    switch (codec) {
    case EAF_SENDPIN_CODEC_FLAC:
        return EAF_CODEC_FLAC;
    case EAF_SENDPIN_CODEC_OPUS:
        return EAF_CODEC_OPUS;
    case EAF_SENDPIN_CODEC_MP3:
        return EAF_CODEC_MP3;
    case EAF_SENDPIN_CODEC_VORBIS:
        return EAF_CODEC_VORBIS;
    default:
        return EAF_CODEC_NONE;
    }
}

void eaf_sendspin_player_init(eaf_sendspin_player_t *player, eaf_sendspin_sink_fn sink,
                              void *sink_ctx) {
    *player = (eaf_sendspin_player_t){.sink = sink, .sink_ctx = sink_ctx, .active_ratio = 1.0};
}

void eaf_sendspin_player_set_decoder(eaf_sendspin_player_t *player,
                                     const eaf_sendspin_decoder_t *decoder) {
    if (!player || !decoder || player->active)
        return;
    player->decoder = *decoder;
}

/* Resample one block and hand it to the sink. `*produced` is the number of
   output frames offered; the sink's unaccepted remainder is dropped, matching
   the PCM path. Returns the frames the sink accepted. */
static uint32_t queue_block(eaf_sendspin_player_t *player, const int32_t *samples, uint32_t count,
                            uint32_t *produced) {
    uint32_t produce = count;
    const int32_t *output = samples;
    if (player->active_ratio != 1.0) {
        double desired = (double)count * player->active_ratio + player->resample_credit;
        produce = (uint32_t)desired;
        if (produce > count + 2u)
            produce = count + 2u;
        player->resample_credit = desired - (double)produce;
        (void)eaf_sync_resample(samples, count, player->resample_scratch, produce, 2u);
        output = player->resample_scratch;
    }
    uint32_t written = player->sink(player->sink_ctx, output, produce);
    player->frames_written += written;
    if (written < produce)
        player->frames_dropped += produce - written;
    *produced = produce;
    return written;
}

/* Adapter for eaf_decode_worker: emits every decoded block to the rate-adjusted
   sink. It always reports the whole decoded buffer as consumed, so backpressure
   drops the tail rather than retaining decoded frames (same policy as PCM). */
static uint32_t decode_sink(void *ctx, const int32_t *samples, uint32_t frames) {
    eaf_sendspin_player_t *player = ctx;
    uint32_t index = 0;
    while (index < frames) {
        uint32_t count = frames - index;
        if (count > EAF_SENDPIN_PLAYER_CHUNK_FRAMES)
            count = EAF_SENDPIN_PLAYER_CHUNK_FRAMES;
        uint32_t produced = 0;
        uint32_t written = queue_block(player, samples + (size_t)index * 2u, count, &produced);
        index += count;
        if (written < produced) {
            player->frames_dropped += frames - index;
            break;
        }
    }
    return frames;
}

/* Update the drift controller at most every 100 ms and return the output/input
   ratio. Positive rate_ppm means the sink should receive more frames per input
   chunk, raising the buffered latency toward the target. */
static double update_rate(eaf_sendspin_player_t *player) {
    int64_t now = (int64_t)hal_monotonic_time_us();
    if (player->rate_update_us && now - player->rate_update_us < 100000)
        return eaf_sync_ppm_to_ratio((double)player->rate_ppm);
    double dt = player->rate_update_us ? (double)(now - player->rate_update_us) / 1e6 : 0.0;
    player->rate_update_us = now;
    if (dt <= 0.0)
        return eaf_sync_ppm_to_ratio((double)player->rate_ppm);
    if (player->rate_auto_target) {
        /* Latch the target to the first stable measurements instead of an
           absolute lead the server may not target exactly. */
        if (player->last_latency_us > 0) {
            player->rate_calibration_sum += (double)player->last_latency_us;
            if (++player->rate_calibration >= 8u) {
                player->target_latency_us = player->rate_calibration_sum / 8.0;
                player->rate_auto_target = false;
            }
        }
        return 1.0;
    }
    double error_ms = (player->target_latency_us - (double)player->last_latency_us) / 1000.0;
    /* Ignore sub-millisecond jitter so the integral tracks real drift only. */
    if (error_ms > -1.0 && error_ms < 1.0)
        error_ms = 0.0;
    player->rate_ppm = (int32_t)eaf_sync_controller_update(&player->controller, error_ms, dt);
    return eaf_sync_ppm_to_ratio((double)player->rate_ppm);
}

int eaf_sendspin_player_begin(eaf_sendspin_player_t *player,
                              const eaf_sendspin_time_filter_t *filter,
                              const eaf_sendspin_stream_start_t *start) {
    if (!player || !filter || !start || !player->sink)
        return EAF_INVALID;
    if (player->active)
        return EAF_STATE;
    if (start->channels != 1u && start->channels != 2u)
        return EAF_UNSUPPORTED;
    if (start->sample_rate == 0u)
        return EAF_UNSUPPORTED;
    eaf_decode_worker_close(&player->worker);
    player->compressed = false;
    player->active_ratio = 1.0;
    if (start->codec != EAF_SENDPIN_CODEC_PCM) {
        eaf_codec_t codec = decoder_codec(start->codec);
        if (codec == EAF_CODEC_NONE || !player->decoder.ops || !player->decoder.ctx ||
            !player->decoder.scratch || player->decoder.scratch_frames == 0u)
            return EAF_UNSUPPORTED;
    } else if (start->bit_depth != 16u && start->bit_depth != 24u && start->bit_depth != 32u) {
        return EAF_UNSUPPORTED;
    }

    player->filter = filter;
    player->format = (eaf_format_t){.sample_rate = start->sample_rate,
                                    .num_channels = 2,
                                    .channel_mask = EAF_CH_FRONT_LEFT | EAF_CH_FRONT_RIGHT};
    player->input_channels = start->channels;
    player->input_bits = start->bit_depth ? start->bit_depth : 16u;
    player->active = true;
    player->synchronized = false;
    player->last_latency_us = 0;
    player->frames_written = player->frames_dropped = player->chunks = 0;
    player->rate_ppm = 0;
    player->rate_update_us = 0;
    player->resample_credit = 0.0;
    player->rate_calibration = 0;
    player->rate_calibration_sum = 0.0;
    eaf_sync_controller_reset(&player->controller);

    if (start->codec != EAF_SENDPIN_CODEC_PCM) {
        eaf_decoder_t adapter = {.ops = player->decoder.ops, .ctx = player->decoder.ctx};
        if (eaf_decode_worker_init(&player->worker, &adapter, decode_sink, player,
                                   player->decoder.scratch, player->decoder.scratch_frames,
                                   &player->format))
            return EAF_INVALID;
        const eaf_decoder_config_t cfg = {.codec = decoder_codec(start->codec),
                                          .sample_rate = start->sample_rate,
                                          .channels = start->channels,
                                          .bit_depth = start->bit_depth,
                                          .extra = (const uint8_t *)start->codec_header,
                                          .extra_length = start->codec_header_length};
        int rc = eaf_decode_worker_open(&player->worker, &cfg);
        if (rc) {
            player->active = false;
            return rc;
        }
        player->compressed = true;
    }
    return EAF_OK;
}

void eaf_sendspin_player_set_rate_control(eaf_sendspin_player_t *player, double target_latency_ms) {
    if (!player)
        return;
    player->rate_control = true;
    player->rate_auto_target = target_latency_ms <= 0.0;
    player->target_latency_us = player->rate_auto_target ? 0.0 : target_latency_ms * 1000.0;
    player->rate_ppm = 0;
    player->rate_update_us = 0;
    player->resample_credit = 0.0;
    player->rate_calibration = 0;
    player->rate_calibration_sum = 0.0;
    /* Gains are a starting point; the actuator limit bounds the correction.
       A slow integral avoids winding up on the small systematic offset between
       the latched target and the server's steady state. */
    eaf_sync_controller_init(&player->controller, 2.0, 0.1, 300.0);
}

/* Feed compressed input to the decoder, draining decoded output when the
   adapter's input ring applies backpressure. Returns a decoder error, if any. */
static int write_compressed(eaf_sendspin_player_t *player, const uint8_t *data, size_t length) {
    size_t offset = 0;
    for (unsigned guard = 0; offset < length && guard < EAF_SENDPIN_DECODE_STEPS; ++guard) {
        if (!eaf_decode_worker_can_push(&player->worker)) {
            int rc = eaf_decode_worker_step(&player->worker);
            if (rc)
                return rc;
            continue;
        }
        size_t consumed = 0;
        int rc = eaf_decode_worker_push(&player->worker, data + offset, length - offset, &consumed);
        if (rc)
            return rc;
        offset += consumed;
        if (consumed == 0u) {
            rc = eaf_decode_worker_step(&player->worker);
            if (rc)
                return rc;
        }
    }
    for (unsigned i = 0; i < EAF_SENDPIN_DECODE_STEPS; ++i) {
        uint32_t before = player->worker.frames_written;
        int rc = eaf_decode_worker_step(&player->worker);
        if (rc)
            return rc;
        if (player->worker.frames_written == before && eaf_decode_worker_can_push(&player->worker))
            break;
    }
    return EAF_OK;
}

int eaf_sendspin_player_write(eaf_sendspin_player_t *player, int64_t server_timestamp_us,
                              const uint8_t *pcm, size_t length) {
    if (!player || !player->active || !pcm)
        return EAF_INVALID;
    player->synchronized = eaf_sendspin_time_synchronized(player->filter);
    if (player->synchronized) {
        uint64_t now = hal_monotonic_time_us();
        int64_t play = eaf_sendspin_compute_client_time(player->filter, server_timestamp_us);
        player->last_latency_us = play - (int64_t)now;
        /* Hard-sync late drop is opt-in: a jump in the time filter can otherwise
           starve the reservoir and drop every subsequent chunk. Phase 3 owns
           scheduling; Phase 2 only reports latency. */
        if (player->drop_late && play < (int64_t)now) {
            if (!player->compressed) {
                size_t frame_bytes =
                    (size_t)player->input_channels * (size_t)(player->input_bits / 8u);
                if (frame_bytes)
                    player->frames_dropped += (uint32_t)(length / frame_bytes);
            }
            player->chunks += 1u;
            return EAF_OK;
        }
    }

    player->active_ratio =
        (player->rate_control && player->synchronized) ? update_rate(player) : 1.0;
    if (player->compressed) {
        int rc = write_compressed(player, pcm, length);
        player->chunks += 1u;
        return rc;
    }

    size_t sample_bytes = player->input_bits / 8u;
    size_t frame_bytes = (size_t)player->input_channels * sample_bytes;
    if (length < frame_bytes)
        return EAF_OK;
    uint32_t frames = (uint32_t)(length / frame_bytes);
    int32_t scratch[EAF_SENDPIN_PLAYER_CHUNK_FRAMES * 2u];
    uint32_t index = 0;
    while (index < frames) {
        uint32_t count = frames - index;
        if (count > EAF_SENDPIN_PLAYER_CHUNK_FRAMES)
            count = EAF_SENDPIN_PLAYER_CHUNK_FRAMES;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t *frame = pcm + (size_t)(index + i) * frame_bytes;
            int32_t left = sample_to_q31(frame, player->input_bits);
            int32_t right = player->input_channels == 2u
                                ? sample_to_q31(frame + sample_bytes, player->input_bits)
                                : left;
            scratch[(size_t)2u * i] = left;
            scratch[(size_t)2u * i + 1u] = right;
        }
        uint32_t produced = 0;
        uint32_t written = queue_block(player, scratch, count, &produced);
        if (written < produced)
            break;
        index += count;
    }
    player->chunks += 1u;
    return EAF_OK;
}

void eaf_sendspin_player_finish(eaf_sendspin_player_t *player) {
    if (!player)
        return;
    eaf_decode_worker_close(&player->worker);
    player->compressed = false;
    player->active = false;
}
