#include "tone_check.h"
#include <eaf/eaf_dec_opus.h>
#include <eaf/eaf_decoder.h>
#include <math.h>
#include <opus.h>
#include <stdio.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define RATE 48000
#define CHANNELS 2
#define FRAME 960 /* 20 ms */
#define BLOCKS 5
#define TOTAL_FRAMES 4800u /* FRAME * BLOCKS */
#define TWO_PI 6.283185307179586476925286766559

int main(void) {
    static int16_t input[(size_t)TOTAL_FRAMES * CHANNELS];
    for (size_t i = 0; i < TOTAL_FRAMES; ++i) {
        double angle = TWO_PI * 1000.0 * (double)i / (double)RATE;
        int16_t value = (int16_t)(sin(angle) * 12000.0);
        input[i * CHANNELS] = value;
        input[i * CHANNELS + 1u] = (int16_t)(-value);
    }

    int error = 0;
    OpusEncoder *encoder = opus_encoder_create(RATE, CHANNELS, OPUS_APPLICATION_AUDIO, &error);
    CHECK(encoder != NULL && error == OPUS_OK);
    (void)opus_encoder_ctl(encoder, OPUS_SET_BITRATE(128000));

    static eaf_dec_opus_t state;
    eaf_decoder_t decoder = {.ops = &eaf_dec_opus_ops, .ctx = &state};
    eaf_format_t output = {.sample_rate = RATE,
                           .num_channels = 2,
                           .channel_mask = EAF_CH_FRONT_LEFT | EAF_CH_FRONT_RIGHT};
    eaf_decoder_config_t config = {.codec = EAF_CODEC_OPUS, .sample_rate = RATE, .channels = 2};
    CHECK(eaf_decoder_open(&decoder, &config, &output) == EAF_OK);

    static int32_t decoded[(size_t)TOTAL_FRAMES * CHANNELS];
    uint32_t decoded_count = 0;
    uint8_t packet[4000];
    for (int block = 0; block < BLOCKS; ++block) {
        int bytes = opus_encode(encoder, input + (size_t)block * FRAME * CHANNELS, FRAME, packet,
                                (opus_int32)sizeof(packet));
        CHECK(bytes > 0);
        size_t consumed = 0;
        CHECK(eaf_decoder_push(&decoder, packet, (size_t)bytes, &consumed) == EAF_OK);
        CHECK(consumed == (size_t)bytes);
        for (;;) {
            uint32_t frames = 0;
            eaf_format_t format = {0};
            uint32_t capacity = TOTAL_FRAMES - decoded_count;
            if (capacity == 0u)
                break;
            CHECK(eaf_decoder_pull(&decoder, decoded + (size_t)decoded_count * 2u, capacity,
                                   &frames, &format) == EAF_OK);
            if (frames == 0u)
                break;
            CHECK(eaf_format_equal(&format, &output));
            decoded_count += frames;
        }
    }
    CHECK(decoded_count == TOTAL_FRAMES);

    /* Opus is lossy and introduces lookahead, so compare energy rather than
       samples: a correct decode preserves the signal power. */
    double input_energy = 0.0;
    double output_energy = 0.0;
    for (size_t i = 0; i < TOTAL_FRAMES; ++i) {
        double original = (double)input[i * CHANNELS];
        double restored = (double)(decoded[i * CHANNELS] >> 16);
        input_energy += original * original;
        output_energy += restored * restored;
    }
    CHECK(input_energy > 0.0);
    double ratio = output_energy / input_energy;
    CHECK(ratio > 0.5 && ratio < 2.0);
    /* The 1 kHz tone must survive the round trip as the dominant component. */
    double dominant = eaf_test_dominant_hz(decoded, decoded_count, 2, 0, RATE, 800.0, 1200.0, 2.0);
    CHECK(dominant > 950.0 && dominant < 1050.0);

    eaf_decoder_close(&decoder);
    opus_encoder_destroy(encoder);
    printf("opus decode ok (%u frames, energy ratio %.3f, %.1f Hz)\n", decoded_count, ratio,
           dominant);
    return 0;
}
