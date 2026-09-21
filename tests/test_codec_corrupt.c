#include <eaf/eaf_dec_flac.h>
#include <eaf/eaf_dec_mp3.h>
#include <eaf/eaf_dec_opus.h>
#include <eaf/eaf_dec_vorbis.h>
#include <eaf/eaf_decoder.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

/* Feed input to an adapter with strictly bounded pulls. The point is not the
   return code (malformed data may legitimately fail) but that no adapter reads
   or writes out of bounds, loops, or leaks. Run under ASan/UBSan in CI. */
static void exercise(const eaf_decoder_ops_t *ops, void *ctx, eaf_codec_t codec, uint32_t rate,
                     const uint8_t *data, size_t length) {
    eaf_decoder_t decoder = {.ops = ops, .ctx = ctx};
    eaf_format_t output = {.sample_rate = rate,
                           .num_channels = 2,
                           .channel_mask = EAF_CH_FRONT_LEFT | EAF_CH_FRONT_RIGHT};
    eaf_decoder_config_t config = {.codec = codec, .sample_rate = rate, .channels = 2};
    static int32_t pcm[4096u * 2u];
    if (eaf_decoder_open(&decoder, &config, &output) != EAF_OK)
        return;
    size_t offset = 0;
    for (unsigned guard = 0; offset < length && guard < 4096u; ++guard) {
        size_t consumed = 0;
        if (eaf_decoder_push(&decoder, data + offset, length - offset, &consumed) != EAF_OK)
            break;
        offset += consumed;
        if (consumed == 0u)
            break;
        for (unsigned pulls = 0; pulls < 4u; ++pulls) {
            uint32_t frames = 0;
            eaf_format_t format = {0};
            if (eaf_decoder_pull(&decoder, pcm, 4096u, &frames, &format) != EAF_OK)
                break;
            if (frames == 0u)
                break;
        }
    }
    eaf_decoder_close(&decoder);
}

static size_t load(const char *path, uint8_t *dst, size_t capacity) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "cannot open %s\n", path);
        return 0;
    }
    size_t length = fread(dst, 1, capacity, file);
    fclose(file);
    return length;
}

static void run_all(uint32_t rate, const uint8_t *data, size_t length) {
    static eaf_dec_flac_t flac;
    static eaf_dec_mp3_t mp3;
    static eaf_dec_vorbis_t vorbis;
    static eaf_dec_opus_t opus;
    exercise(&eaf_dec_flac_ops, &flac, EAF_CODEC_FLAC, rate, data, length);
    exercise(&eaf_dec_mp3_ops, &mp3, EAF_CODEC_MP3, rate, data, length);
    exercise(&eaf_dec_vorbis_ops, &vorbis, EAF_CODEC_VORBIS, rate, data, length);
    exercise(&eaf_dec_opus_ops, &opus, EAF_CODEC_OPUS, rate, data, length);
}

int main(int argc, char **argv) {
    CHECK(argc >= 4);
    static uint8_t fixture[65536];

    /* Pure garbage. */
    uint8_t garbage[512];
    memset(garbage, 0xAA, sizeof(garbage));
    run_all(44100, garbage, sizeof(garbage));
    memset(garbage, 0x00, sizeof(garbage));
    run_all(48000, garbage, sizeof(garbage));

    /* Each real stream truncated at several points. */
    const char *paths[3] = {argv[1], argv[2], argv[3]};
    for (size_t i = 0; i < 3; ++i) {
        size_t length = load(paths[i], fixture, sizeof(fixture));
        CHECK(length > 0);
        for (size_t fraction = 1; fraction <= 4; ++fraction) {
            size_t truncated = length / (fraction + 1u);
            run_all(44100, fixture, truncated);
        }
    }

    /* Deterministic bit flips in each stream header. */
    size_t length = load(paths[0], fixture, sizeof(fixture));
    CHECK(length > 64);
    for (size_t i = 0; i < 64; i += 7)
        fixture[i] ^= (uint8_t)(0x5Au + i);
    run_all(44100, fixture, length);

    puts("codec corruption guard PASS");
    return 0;
}
