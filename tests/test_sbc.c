#include "check.h"
#include <eaf/eaf_sbc_oi.h>
#include <stdio.h>
#include <string.h>
static eaf_sbc_oi_t codec;
static eaf_bt_ingress_t queue;
static eaf_bt_decoder_t worker;
int main(int argc, char **argv) {
    CHECK(argc == 2);
    uint8_t payload[1025] = {4};
    FILE *file = fopen(argv[1], "rb");
    CHECK(file);
    size_t bytes = fread(payload + 1, 1, 1024, file);
    CHECK(bytes > 0 && bytes < 1024 && !ferror(file));
    fclose(file);
    eaf_sbc_decoder_t decoder;
    CHECK(eaf_sbc_oi_init(&codec, &decoder) == 0);
    size_t consumed = 0;
    uint32_t frames = 0;
    eaf_format_t fmt = {0};
    int16_t pcm[256];
    CHECK(decoder.frame(decoder.ctx, payload + 1, bytes, &consumed, pcm, &frames, &fmt) == 0);
    CHECK(consumed > 0 && frames == 128 && fmt.sample_rate == 48000 && fmt.num_channels == 2);
    uint8_t saved = payload[4];
    payload[4] ^= 1; /* SBC CRC byte */
    CHECK(decoder.reset(decoder.ctx) == 0);
    CHECK(decoder.frame(decoder.ctx, payload + 1, bytes, &consumed, pcm, &frames, &fmt) ==
          EAF_INVALID);
    payload[4] = saved;
    eaf_reservoir_t r;
    int32_t storage[128];
    eaf_format_t stereo = {48000, 2, 3};
    CHECK(eaf_reservoir_init(&r, storage, 64, stereo, 32) == 0);
    eaf_bt_ingress_init(&queue);
    CHECK(eaf_bt_decoder_init(&worker, &queue, &r, &decoder) == 0);
    CHECK(eaf_bt_sbc_receive(&queue, 1, 0, payload, bytes + 1u) == 0);
    CHECK(eaf_bt_decoder_step(&worker) == 0 && worker.pending == 128 && worker.sent == 64);
    for (unsigned i = 0; i < 5; ++i)
        CHECK(eaf_bt_decoder_step(&worker) == 0 && worker.sent == 64);
    uint32_t total = 0;
    bool nonzero = false;
    int32_t out[64];
    eaf_buffer_t buffer = {out, 32, 32, 64, stereo, 0};
    for (unsigned step = 0; step < 1000 && total < 512; ++step) {
        CHECK(eaf_bt_decoder_step(&worker) == 0);
        if (eaf_reservoir_level(&r) >= 32) {
            CHECK(eaf_reservoir_pull(&r, &buffer) == 0);
            for (size_t i = 0; i < 64; ++i) {
                CHECK(out[i] % 65536 == 0);
                if (out[i])
                    nonzero = true;
            }
            total += 32;
        }
    }
    CHECK(total == 512 && nonzero && !worker.rejected && !r.underruns);
    return 0;
}
