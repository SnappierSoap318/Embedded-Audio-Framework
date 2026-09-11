#include <eaf/eaf_wav.h>
#include <string.h>
static uint16_t u16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}
static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int read_frames(eaf_source_t *source, int32_t *samples, uint32_t capacity,
                       uint32_t *frames) {
    if (!source || !source->ctx || !samples || !capacity || !frames)
        return EAF_INVALID;
    eaf_wav_t *w = source->ctx;
    *frames = 0;
    uint64_t left = w->total_frames - w->position;
    uint32_t take = capacity < EAF_WAV_BLOCK_FRAMES ? capacity : EAF_WAV_BLOCK_FRAMES;
    if (left < take)
        take = (uint32_t)left;
    if (!take)
        return EAF_OK;
    int rc = w->reader.read_at(w->reader.ctx, w->data_offset + w->position * w->block_align,
                               w->scratch, (size_t)take * w->block_align);
    if (rc)
        return rc;
    size_t width = w->bits / 8u;
    for (size_t i = 0; i < (size_t)take * w->format.num_channels; ++i) {
        const unsigned char *p = w->scratch + i * width;
        uint32_t raw = 0;
        for (size_t byte = 0; byte < width; ++byte)
            raw |= (uint32_t)p[byte] << (byte * 8u);
        int64_t signed_sample = raw;
        if (raw & (UINT32_C(1) << (w->bits - 1u)))
            signed_sample -= INT64_C(1) << w->bits;
        samples[i] = (int32_t)(signed_sample * (INT64_C(1) << (32u - w->bits)));
    }
    w->position += take;
    *frames = take;
    return EAF_OK;
}
static int seek_frame(eaf_source_t *source, uint64_t frame) {
    if (!source || !source->ctx)
        return EAF_INVALID;
    eaf_wav_t *w = source->ctx;
    if (frame > w->total_frames)
        return EAF_INVALID;
    w->position = frame;
    return EAF_OK;
}
static const struct eaf_source_ops ops = {read_frames, seek_frame};
int eaf_wav_open(eaf_wav_t *w, eaf_source_t *source, const eaf_reader_t *reader) {
    if (!w || !source || !reader || !reader->read_at || reader->size < 12u)
        return EAF_INVALID;
    unsigned char header[16];
    int rc = reader->read_at(reader->ctx, 0, header, 12);
    if (rc)
        return rc;
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0)
        return EAF_UNSUPPORTED;
    uint64_t end = (uint64_t)u32(header + 4) + 8u;
    if (end < 12u || end > reader->size)
        return EAF_INVALID;
    eaf_format_t fmt = {0};
    uint16_t bits = 0, align = 0;
    uint64_t data_offset = 0, data_size = 0;
    bool have_fmt = false, have_data = false;
    for (uint64_t offset = 12; offset < end;) {
        if (end - offset < 8u)
            return EAF_INVALID;
        rc = reader->read_at(reader->ctx, offset, header, 8);
        if (rc)
            return rc;
        uint32_t size = u32(header + 4);
        uint64_t padded = (uint64_t)size + (size & 1u);
        offset += 8u;
        if (padded > end - offset)
            return EAF_INVALID;
        if (!memcmp(header, "fmt ", 4)) {
            if (have_fmt || size < 16u)
                return EAF_INVALID;
            rc = reader->read_at(reader->ctx, offset, header, 16);
            if (rc)
                return rc;
            if (u16(header) != 1u)
                return EAF_UNSUPPORTED;
            uint16_t channels = u16(header + 2);
            if (channels != 1u && channels != 2u)
                return EAF_UNSUPPORTED;
            bits = u16(header + 14);
            align = u16(header + 12);
            if (bits != 16u && bits != 24u && bits != 32u)
                return EAF_UNSUPPORTED;
            fmt = (eaf_format_t){u32(header + 4), (uint8_t)channels,
                                 channels == 1u ? EAF_CH_FRONT_CENTER : 3u};
            if (!eaf_format_valid(&fmt) || align != channels * (bits / 8u) ||
                (uint64_t)fmt.sample_rate * align != u32(header + 8))
                return EAF_INVALID;
            have_fmt = true;
        } else if (!memcmp(header, "data", 4)) {
            if (have_data)
                return EAF_INVALID;
            data_offset = offset;
            data_size = size;
            have_data = true;
        }
        offset += padded;
    }
    if (!have_fmt || !have_data || data_size % align)
        return EAF_INVALID;
    *w = (eaf_wav_t){.reader = *reader,
                     .data_offset = data_offset,
                     .total_frames = data_size / align,
                     .format = fmt,
                     .bits = bits,
                     .block_align = align};
    *source = (eaf_source_t){&ops, w, fmt, w->total_frames};
    return EAF_OK;
}
