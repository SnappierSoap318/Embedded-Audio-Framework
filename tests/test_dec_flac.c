#include <eaf/eaf_dec_flac.h>
#include <eaf/eaf_decoder.h>
#include <stdio.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

/* The fixture is a 4096-frame 16-bit stereo ramp: L[i] = (int16_t)(i * 101),
   R[i] = -L[i]. dr_flac's s32 output is left-justified to Q1.31. */
static int16_t expected(int32_t index, int sign) {
    int32_t value = index * 101 * sign;
    return (int16_t)value;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <tone.flac>\n", argv[0]);
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

    static eaf_dec_flac_t state;
    eaf_dec_flac_configure(&state, NULL, NULL);
    eaf_decoder_t decoder = {.ops = &eaf_dec_flac_ops, .ctx = &state};
    eaf_format_t output = {.sample_rate = 44100,
                           .num_channels = 2,
                           .channel_mask = EAF_CH_FRONT_LEFT | EAF_CH_FRONT_RIGHT};
    eaf_decoder_config_t config = {.codec = EAF_CODEC_FLAC, .sample_rate = 44100, .channels = 2};
    CHECK(eaf_decoder_open(&decoder, &config, &output) == EAF_OK);

    /* Nothing is decodable until the stream is fully buffered. */
    uint32_t frames = 0;
    eaf_format_t format = {0};
    static int32_t pcm[2048];
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
    CHECK(pushed == length);
    eaf_dec_flac_finish(&state);

    uint32_t total = 0;
    for (;;) {
        CHECK(eaf_decoder_pull(&decoder, pcm, 1024, &frames, &format) == EAF_OK);
        if (frames == 0)
            break;
        CHECK(eaf_format_equal(&format, &output));
        for (uint32_t i = 0; i < frames; ++i) {
            size_t index = (size_t)i * 2u;
            CHECK(pcm[index] == eaf_pcm16_to_q31(expected((int32_t)(total + i), 1)));
            CHECK(pcm[index + 1u] == eaf_pcm16_to_q31(expected((int32_t)(total + i), -1)));
        }
        total += frames;
        if (total > 8192u)
            break;
    }
    CHECK(total == 4096u);
    eaf_decoder_close(&decoder);
    printf("flac decode ok (%u frames)\n", total);
    return 0;
}
