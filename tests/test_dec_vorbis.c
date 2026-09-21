#include "tone_check.h"
#include <eaf/eaf_dec_vorbis.h>
#include <eaf/eaf_decoder.h>
#include <stdio.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

/* Same full-scale anti-phase sawtooth as the other fixtures. */
static int16_t expected(int32_t index, int sign) {
    return (int16_t)(index * 101 * sign);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <tone.ogg>\n", argv[0]);
        return 2;
    }
    FILE *file = fopen(argv[1], "rb");
    if (!file) {
        perror("open");
        return 2;
    }
    static uint8_t compressed[65536];
    size_t length = fread(compressed, 1, sizeof(compressed), file);
    fclose(file);
    CHECK(length > 0);

    static eaf_dec_vorbis_t state;
    eaf_decoder_t decoder = {.ops = &eaf_dec_vorbis_ops, .ctx = &state};
    eaf_format_t output = {.sample_rate = 44100,
                           .num_channels = 2,
                           .channel_mask = EAF_CH_FRONT_LEFT | EAF_CH_FRONT_RIGHT};
    eaf_decoder_config_t config = {.codec = EAF_CODEC_VORBIS, .sample_rate = 44100, .channels = 2};
    CHECK(eaf_decoder_open(&decoder, &config, &output) == EAF_OK);

    size_t pushed = 0;
    while (pushed < length) {
        size_t consumed = 0;
        CHECK(eaf_decoder_push(&decoder, compressed + pushed, length - pushed, &consumed) ==
              EAF_OK);
        CHECK(consumed > 0);
        pushed += consumed;
    }

    static int32_t pcm[8192 * 2];
    static int32_t decoded[20000u * 2u];
    uint32_t frames = 0;
    eaf_format_t format = {0};
    uint32_t total = 0;
    for (;;) {
        CHECK(eaf_decoder_pull(&decoder, pcm, 8192, &frames, &format) == EAF_OK);
        if (frames == 0)
            break;
        CHECK(eaf_format_equal(&format, &output));
        CHECK(total + frames <= 20000u);
        for (uint32_t i = 0; i < frames * 2u; ++i)
            decoded[(size_t)total * 2u + i] = pcm[i];
        total += frames;
        if (total > 20000u)
            break;
    }
    /* Vorbis adds delay/padding, so allow a frame-count range. */
    CHECK(total > 3000u && total < 5000u);

    /* As for MP3: inverted channels, preserved fundamental and signal power. */
    for (uint32_t i = 0; i < total; ++i) {
        int64_t sum = (int64_t)decoded[(size_t)i * 2u] + decoded[(size_t)i * 2u + 1u];
        CHECK(sum <= 524288 && sum >= -524288); /* 8 LSB at 16-bit = 8 * 65536 */
    }
    double dominant = eaf_test_dominant_hz(decoded, total, 2, 0, 44100, 40.0, 120.0, 0.5);
    CHECK(dominant > 50.0 && dominant < 90.0);

    double source_energy = 0.0;
    for (uint32_t i = 0; i < total; ++i)
        source_energy +=
            (double)expected((int32_t)i, 1) * 65536.0 * expected((int32_t)i, 1) * 65536.0;
    double source_rms = sqrt(source_energy / total);
    double decoded_rms = eaf_test_rms(decoded, total, 2, 0);
    CHECK(decoded_rms > source_rms * 0.5 && decoded_rms < source_rms * 1.5);

    eaf_decoder_close(&decoder);
    printf("vorbis decode ok (%u frames, %.1f Hz)\n", total, dominant);
    return 0;
}
