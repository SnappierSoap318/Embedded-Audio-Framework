#include <eaf/eaf_dec_mp3.h>
#include <eaf/eaf_decoder.h>
#include <stdio.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <tone.mp3>\n", argv[0]);
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

    static eaf_dec_mp3_t state;
    eaf_dec_mp3_configure(&state, NULL, NULL);
    eaf_decoder_t decoder = {.ops = &eaf_dec_mp3_ops, .ctx = &state};
    eaf_format_t output = {.sample_rate = 44100,
                           .num_channels = 2,
                           .channel_mask = EAF_CH_FRONT_LEFT | EAF_CH_FRONT_RIGHT};
    eaf_decoder_config_t config = {.codec = EAF_CODEC_MP3, .sample_rate = 44100, .channels = 2};
    CHECK(eaf_decoder_open(&decoder, &config, &output) == EAF_OK);

    uint32_t frames = 0;
    eaf_format_t format = {0};
    static int32_t pcm[4096];
    CHECK(eaf_decoder_pull(&decoder, pcm, 1024, &frames, &format) == EAF_OK);
    CHECK(frames == 0);

    size_t pushed = 0;
    while (pushed < length) {
        size_t consumed = 0;
        CHECK(eaf_decoder_push(&decoder, compressed + pushed, length - pushed, &consumed) ==
              EAF_OK);
        CHECK(consumed > 0);
        pushed += consumed;
    }
    eaf_dec_mp3_finish(&state);

    double energy = 0.0;
    uint32_t total = 0;
    for (;;) {
        CHECK(eaf_decoder_pull(&decoder, pcm, 1024, &frames, &format) == EAF_OK);
        if (frames == 0)
            break;
        CHECK(eaf_format_equal(&format, &output));
        for (uint32_t i = 0; i < frames; ++i) {
            double restored = (double)(pcm[2u * (size_t)i] >> 16);
            energy += restored * restored;
        }
        total += frames;
        if (total > 20000u)
            break;
    }
    /* Lossy, and the encoder adds delay/padding, so allow a range. */
    CHECK(total > 3500u && total < 4600u);
    CHECK(energy > 0.0);
    eaf_decoder_close(&decoder);
    printf("mp3 decode ok (%u frames)\n", total);
    return 0;
}
