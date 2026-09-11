#include "check.h"
#include <eaf/eaf_wav.h>
#include <string.h>
typedef struct {
    unsigned char bytes[128];
    size_t size;
    bool fail;
} memory_file;
static int read_at(void *ctx, uint64_t offset, void *dst, size_t bytes) {
    memory_file *m = ctx;
    if (m->fail || offset > m->size || bytes > m->size - offset)
        return EAF_IO;
    memcpy(dst, m->bytes + offset, bytes);
    return EAF_OK;
}
static void put16(unsigned char *p, uint16_t x) {
    p[0] = (unsigned char)x;
    p[1] = (unsigned char)(x >> 8);
}
static void put32(unsigned char *p, uint32_t x) {
    for (size_t i = 0; i < 4; ++i)
        p[i] = (unsigned char)(x >> (i * 8u));
}
static void make(memory_file *m, uint16_t bits, uint16_t channels) {
    *m = (memory_file){0};
    unsigned char *b = m->bytes;
    memcpy(b, "RIFF", 4);
    memcpy(b + 8, "WAVEJUNK", 8);
    put32(b + 16, 1);
    b[20] = 99;
    b[21] = 0; /* odd chunk with padding */
    memcpy(b + 22, "fmt ", 4);
    put32(b + 26, 16);
    put16(b + 30, 1);
    put16(b + 32, channels);
    put32(b + 34, 48000);
    uint16_t align = (uint16_t)(channels * (bits / 8u));
    put32(b + 38, 48000u * align);
    put16(b + 42, align);
    put16(b + 44, bits);
    memcpy(b + 46, "data", 4);
    uint32_t bytes = 3u * align;
    put32(b + 50, bytes);
    m->size = 54u + bytes + (bytes & 1u);
    put32(b + 4, (uint32_t)m->size - 8u);
    size_t width = bits / 8u;
    uint32_t values[3] = {UINT32_C(1) << (bits - 1u), (UINT32_C(1) << (bits - 1u)) - 1u, 0u};
    for (size_t frame = 0; frame < 3; ++frame)
        for (size_t ch = 0; ch < channels; ++ch)
            for (size_t byte = 0; byte < width; ++byte)
                b[54u + (frame * channels + ch) * width + byte] =
                    (unsigned char)(values[frame] >> (byte * 8u));
}
int main(void) {
    memory_file m;
    eaf_wav_t w;
    eaf_source_t s;
    eaf_reader_t reader = {read_at, &m, 0};
    for (uint16_t bits = 16; bits <= 32; bits = (uint16_t)(bits + 8u)) {
        for (uint16_t channels = 1; channels <= 2; ++channels) {
            make(&m, bits, channels);
            reader.size = m.size;
            CHECK(eaf_wav_open(&w, &s, &reader) == 0);
            CHECK(s.total_frames == 3 && s.format.num_channels == channels);
            CHECK(s.format.channel_mask == (channels == 1u ? EAF_CH_FRONT_CENTER : 3u));
            int32_t out[6] = {0};
            uint32_t n = 0;
            CHECK(s.ops->read(&s, out, 3, &n) == 0 && n == 3);
            for (size_t ch = 0; ch < channels; ++ch) {
                CHECK(out[ch] == INT32_MIN);
                CHECK(out[channels + ch] ==
                      INT32_MAX - (int32_t)((UINT32_C(1) << (32u - bits)) - 1u));
                CHECK(out[(size_t)2u * channels + ch] == 0);
            }
            CHECK(s.ops->read(&s, out, 3, &n) == 0 && n == 0);
            CHECK(s.ops->seek(&s, 1) == 0);
            CHECK(s.ops->read(&s, out, 1, &n) == 0 && n == 1 && out[0] > 0);
            CHECK(s.ops->seek(&s, 4) == EAF_INVALID && w.position == 2);
            m.fail = true;
            CHECK(s.ops->read(&s, out, 1, &n) == EAF_IO && n == 0 && w.position == 2);
        }
    }
    make(&m, 16, 2);
    reader.size = m.size;
    put16(m.bytes + 30, 3);
    CHECK(eaf_wav_open(&w, &s, &reader) == EAF_UNSUPPORTED);
    put16(m.bytes + 30, 1);
    put16(m.bytes + 42, 1);
    CHECK(eaf_wav_open(&w, &s, &reader) == EAF_INVALID);
    make(&m, 16, 2);
    reader.size = m.size - 1u;
    CHECK(eaf_wav_open(&w, &s, &reader) == EAF_INVALID);
    reader.size = m.size;
    put32(m.bytes + 50, UINT32_MAX);
    CHECK(eaf_wav_open(&w, &s, &reader) == EAF_INVALID);
    make(&m, 16, 2);
    put32(m.bytes + 50, 11);
    CHECK(eaf_wav_open(&w, &s, &reader) == EAF_INVALID);
    make(&m, 16, 2);
    put32(m.bytes + 50, 0);
    m.size = 54;
    put32(m.bytes + 4, 46);
    reader.size = m.size;
    CHECK(eaf_wav_open(&w, &s, &reader) == 0 && s.total_frames == 0);
    return 0;
}
