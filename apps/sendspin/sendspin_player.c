#include <eaf/eaf_hal.h>
#include <eaf/eaf_sendspin_player.h>

static int16_t read_le16(const uint8_t *p) {
    uint16_t value = (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
    return (int16_t)value;
}

void eaf_sendspin_player_init(eaf_sendspin_player_t *player, int32_t *storage,
                              uint32_t capacity_frames) {
    *player = (eaf_sendspin_player_t){.storage = storage, .capacity_frames = capacity_frames};
}

int eaf_sendspin_player_begin(eaf_sendspin_player_t *player,
                              const eaf_sendspin_time_filter_t *filter,
                              const eaf_sendspin_stream_start_t *start) {
    if (!player || !filter || !start)
        return EAF_INVALID;
    if (player->active)
        return EAF_STATE;
    if (start->codec != EAF_SENDPIN_CODEC_PCM || start->bit_depth != 16u ||
        (start->channels != 1u && start->channels != 2u) || start->sample_rate == 0u)
        return EAF_UNSUPPORTED;
    eaf_format_t format = {.sample_rate = start->sample_rate,
                           .num_channels = 2,
                           .channel_mask = EAF_CH_FRONT_LEFT | EAF_CH_FRONT_RIGHT};
    int rc = eaf_reservoir_init(&player->reservoir, player->storage, player->capacity_frames,
                                format, player->capacity_frames * 3u / 4u);
    if (rc)
        return rc;
    player->filter = filter;
    player->format = format;
    player->input_channels = start->channels;
    player->active = true;
    player->synchronized = false;
    player->last_latency_us = 0;
    player->frames_written = player->frames_dropped = player->chunks = 0;
    return EAF_OK;
}

int eaf_sendspin_player_write(eaf_sendspin_player_t *player, int64_t server_timestamp_us,
                              const uint8_t *pcm, size_t length) {
    if (!player || !player->active || !pcm)
        return EAF_INVALID;
    size_t frame_bytes = (size_t)player->input_channels * 2u;
    if (length < frame_bytes)
        return EAF_OK;
    uint32_t frames = (uint32_t)(length / frame_bytes);

    player->synchronized = eaf_sendspin_time_synchronized(player->filter);
    if (player->synchronized) {
        uint64_t now = hal_monotonic_time_us();
        int64_t play = eaf_sendspin_compute_client_time(player->filter, server_timestamp_us);
        player->last_latency_us = play - (int64_t)now;
        if (play + player->drop_ahead_us < (int64_t)now) {
            player->frames_dropped += frames;
            player->chunks += 1u;
            return EAF_OK;
        }
    }

    int32_t scratch[EAF_SENDPIN_PLAYER_CHUNK_FRAMES * 2u];
    uint32_t index = 0;
    while (index < frames) {
        uint32_t count = frames - index;
        if (count > EAF_SENDPIN_PLAYER_CHUNK_FRAMES)
            count = EAF_SENDPIN_PLAYER_CHUNK_FRAMES;
        for (uint32_t i = 0; i < count; ++i) {
            size_t base = (size_t)(index + i) * player->input_channels;
            int16_t left = read_le16(pcm + 2u * base);
            int16_t right = player->input_channels == 2u ? read_le16(pcm + 2u * (base + 1u)) : left;
            scratch[(size_t)2u * i] = eaf_pcm16_to_q31(left);
            scratch[(size_t)2u * i + 1u] = eaf_pcm16_to_q31(right);
        }
        uint32_t written = eaf_reservoir_write(&player->reservoir, scratch, count);
        player->frames_written += written;
        if (written < count) {
            player->frames_dropped += count - written;
            break;
        }
        index += count;
    }
    player->chunks += 1u;
    return EAF_OK;
}

void eaf_sendspin_player_finish(eaf_sendspin_player_t *player) {
    if (!player || !player->active)
        return;
    eaf_reservoir_finish(&player->reservoir);
    player->active = false;
}

uint32_t eaf_sendspin_player_capacity_ms(const eaf_sendspin_player_t *player) {
    if (!player || !player->format.sample_rate)
        return 0;
    return (uint32_t)((uint64_t)player->capacity_frames * 1000u / player->format.sample_rate);
}

uint32_t eaf_sendspin_player_buffer_bytes(const eaf_sendspin_player_t *player) {
    return player ? player->capacity_frames * 2u * 2u : 0u;
}
