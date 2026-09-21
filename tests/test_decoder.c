#include <eaf/eaf_dec_pcm.h>
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

typedef struct {
    bool open;
    uint32_t queue;
    uint32_t next;
    uint32_t resets;
    uint32_t pushed;
    int pull_error;
    uint32_t pull_cap;
    uint8_t report_channels;
    eaf_format_t format;
} mock_t;

static int mock_open(void *ctx, const eaf_decoder_config_t *cfg, const eaf_format_t *output) {
    mock_t *m = ctx;
    if (cfg->codec != EAF_CODEC_PCM)
        return EAF_UNSUPPORTED;
    m->open = true;
    m->queue = 0;
    m->next = 0;
    m->format = *output;
    if (m->report_channels != 0u) {
        m->format.num_channels = m->report_channels;
        m->format.channel_mask =
            m->report_channels == 1u ? EAF_CH_FRONT_CENTER : EAF_CH_FRONT_LEFT | EAF_CH_FRONT_RIGHT;
    }
    return EAF_OK;
}

static int mock_push(void *ctx, const uint8_t *data, size_t length, size_t *consumed) {
    mock_t *m = ctx;
    (void)data;
    m->queue += (uint32_t)length;
    m->pushed += (uint32_t)length;
    *consumed = length;
    return EAF_OK;
}

static int mock_pull(void *ctx, int32_t *pcm, uint32_t max_frames, uint32_t *frames,
                     eaf_format_t *format) {
    mock_t *m = ctx;
    if (m->pull_error != 0) {
        int error = m->pull_error;
        m->pull_error = 0;
        return error;
    }
    uint32_t n = m->queue;
    if (n > max_frames)
        n = max_frames;
    if (m->pull_cap != 0u && n > m->pull_cap)
        n = m->pull_cap;
    for (uint32_t i = 0; i < n; ++i) {
        size_t index = (size_t)i * 2u;
        pcm[index] = (int32_t)m->next;
        pcm[index + 1u] = (int32_t)m->next;
        ++m->next;
    }
    m->queue -= n;
    *frames = n;
    *format = m->format;
    return EAF_OK;
}

static int mock_reset(void *ctx) {
    mock_t *m = ctx;
    m->queue = 0;
    m->next = 0;
    ++m->resets;
    return EAF_OK;
}

static void mock_close(void *ctx) {
    mock_t *m = ctx;
    m->open = false;
}

static const eaf_decoder_ops_t mock_ops = {.open = mock_open,
                                           .push = mock_push,
                                           .pull = mock_pull,
                                           .reset = mock_reset,
                                           .close = mock_close};

typedef struct {
    int32_t samples[2048];
    uint32_t frames;
    uint32_t accept;
} sink_t;

static uint32_t sink_cb(void *ctx, const int32_t *samples, uint32_t frames) {
    sink_t *k = ctx;
    uint32_t take = frames;
    if (k->accept != 0u && take > k->accept)
        take = k->accept;
    size_t base = (size_t)k->frames * 2u;
    for (size_t i = 0; i < (size_t)take * 2u; ++i)
        k->samples[base + i] = samples[i];
    k->frames += take;
    return take;
}

static eaf_format_t output_format(void) {
    return (eaf_format_t){.sample_rate = 44100,
                          .num_channels = 2,
                          .channel_mask = EAF_CH_FRONT_LEFT | EAF_CH_FRONT_RIGHT};
}

static int test_dispatch_validation(void) {
    eaf_decoder_t d = {0};
    eaf_decoder_config_t cfg = {
        .codec = EAF_CODEC_PCM, .sample_rate = 44100, .channels = 2, .bit_depth = 16};
    eaf_format_t out = output_format();
    size_t consumed = 1;
    uint32_t frames = 1;
    eaf_format_t fmt = output_format();
    int32_t sample = 0;
    CHECK(eaf_decoder_open(&d, &cfg, &out) == EAF_INVALID);
    CHECK(eaf_decoder_push(&d, (const uint8_t *)"x", 1, &consumed) == EAF_INVALID);
    CHECK(eaf_decoder_pull(&d, &sample, 1, &frames, &fmt) == EAF_INVALID);
    CHECK(eaf_decoder_reset(&d) == EAF_INVALID);
    eaf_decoder_close(&d);

    mock_t m = {0};
    d = (eaf_decoder_t){.ops = &mock_ops, .ctx = &m};
    CHECK(eaf_decoder_open(&d, &cfg, &out) == EAF_OK);
    CHECK(m.open);
    eaf_decoder_config_t bad = cfg;
    bad.sample_rate = 0;
    CHECK(eaf_decoder_open(&d, &bad, &out) == EAF_INVALID);
    CHECK(eaf_decoder_pull(&d, &sample, 0, &frames, &fmt) == EAF_INVALID);
    CHECK(eaf_decoder_push(&d, NULL, 1, &consumed) == EAF_INVALID);
    eaf_decoder_close(&d);
    CHECK(!m.open);
    return 0;
}

static int test_worker_backpressure(void) {
    mock_t m = {0};
    eaf_decoder_t d = {.ops = &mock_ops, .ctx = &m};
    sink_t k = {.accept = 2};
    int32_t scratch[8 * 2];
    eaf_format_t out = output_format();
    eaf_decode_worker_t w;
    eaf_decoder_config_t cfg = {
        .codec = EAF_CODEC_PCM, .sample_rate = 44100, .channels = 2, .bit_depth = 16};
    size_t consumed = 0;

    CHECK(eaf_decode_worker_init(&w, &d, sink_cb, &k, scratch, 8, &out) == EAF_OK);
    CHECK(!eaf_decode_worker_can_push(&w));
    CHECK(eaf_decode_worker_push(&w, (const uint8_t *)"abc", 3, &consumed) == EAF_STATE);
    CHECK(eaf_decode_worker_open(&w, &cfg) == EAF_OK);

    CHECK(eaf_decode_worker_push(&w, (const uint8_t *)"abcdef", 6, &consumed) == EAF_OK);
    CHECK(consumed == 6);
    CHECK(eaf_decode_worker_can_push(&w));

    CHECK(eaf_decode_worker_step(&w) == EAF_OK);
    CHECK(!eaf_decode_worker_can_push(&w));
    CHECK(k.frames == 2);
    CHECK(eaf_decode_worker_push(&w, (const uint8_t *)"z", 1, &consumed) == EAF_OK);
    CHECK(consumed == 0);

    CHECK(eaf_decode_worker_step(&w) == EAF_OK);
    CHECK(eaf_decode_worker_step(&w) == EAF_OK);
    CHECK(eaf_decode_worker_can_push(&w));
    CHECK(k.frames == 6);
    CHECK(w.frames_written == 6);
    for (uint32_t i = 0; i < 6; ++i) {
        size_t index = (size_t)i * 2u;
        CHECK(k.samples[index] == (int32_t)i);
        CHECK(k.samples[index + 1u] == (int32_t)i);
    }
    eaf_decode_worker_close(&w);
    return 0;
}

static int test_worker_reset_drops_pending(void) {
    mock_t m = {0};
    eaf_decoder_t d = {.ops = &mock_ops, .ctx = &m};
    sink_t k = {.accept = 2};
    int32_t scratch[8 * 2];
    eaf_format_t out = output_format();
    eaf_decode_worker_t w;
    eaf_decoder_config_t cfg = {
        .codec = EAF_CODEC_PCM, .sample_rate = 44100, .channels = 2, .bit_depth = 16};
    size_t consumed = 0;

    CHECK(eaf_decode_worker_init(&w, &d, sink_cb, &k, scratch, 8, &out) == EAF_OK);
    CHECK(eaf_decode_worker_open(&w, &cfg) == EAF_OK);
    CHECK(eaf_decode_worker_push(&w, (const uint8_t *)"abcdef", 6, &consumed) == EAF_OK);
    CHECK(eaf_decode_worker_step(&w) == EAF_OK);
    CHECK(w.pending == 6 && w.sent == 2);
    CHECK(eaf_decode_worker_reset(&w) == EAF_OK);
    CHECK(w.pending == 0 && w.sent == 0);
    CHECK(w.frames_dropped == 4);
    CHECK(eaf_decode_worker_can_push(&w));
    eaf_decode_worker_close(&w);
    return 0;
}

static int test_worker_format_mismatch(void) {
    mock_t m = {0};
    m.report_channels = 1;
    eaf_decoder_t d = {.ops = &mock_ops, .ctx = &m};
    sink_t k = {0};
    int32_t scratch[8 * 2];
    eaf_format_t out = output_format();
    eaf_decode_worker_t w;
    eaf_decoder_config_t cfg = {
        .codec = EAF_CODEC_PCM, .sample_rate = 44100, .channels = 2, .bit_depth = 16};
    size_t consumed = 0;

    CHECK(eaf_decode_worker_init(&w, &d, sink_cb, &k, scratch, 8, &out) == EAF_OK);
    CHECK(eaf_decode_worker_open(&w, &cfg) == EAF_OK);
    CHECK(eaf_decode_worker_push(&w, (const uint8_t *)"ab", 2, &consumed) == EAF_OK);
    CHECK(eaf_decode_worker_step(&w) == EAF_INVALID);
    CHECK(m.resets == 1);
    CHECK(k.frames == 0);
    eaf_decode_worker_close(&w);
    return 0;
}

static int test_worker_pull_error_recovers(void) {
    mock_t m = {0};
    eaf_decoder_t d = {.ops = &mock_ops, .ctx = &m};
    sink_t k = {0};
    int32_t scratch[8 * 2];
    eaf_format_t out = output_format();
    eaf_decode_worker_t w;
    eaf_decoder_config_t cfg = {
        .codec = EAF_CODEC_PCM, .sample_rate = 44100, .channels = 2, .bit_depth = 16};
    size_t consumed = 0;

    CHECK(eaf_decode_worker_init(&w, &d, sink_cb, &k, scratch, 8, &out) == EAF_OK);
    CHECK(eaf_decode_worker_open(&w, &cfg) == EAF_OK);
    CHECK(eaf_decode_worker_push(&w, (const uint8_t *)"ab", 2, &consumed) == EAF_OK);
    m.pull_error = EAF_IO;
    CHECK(eaf_decode_worker_step(&w) == EAF_IO);
    CHECK(m.resets == 1);
    CHECK(eaf_decode_worker_push(&w, (const uint8_t *)"cd", 2, &consumed) == EAF_OK);
    CHECK(eaf_decode_worker_step(&w) == EAF_OK);
    CHECK(k.frames == 2);
    eaf_decode_worker_close(&w);
    return 0;
}

static int test_pcm_adapter(void) {
    static eaf_decoder_pcm_t state;
    eaf_decoder_t d = {.ops = &eaf_decoder_pcm_ops, .ctx = &state};
    eaf_format_t out = output_format();
    eaf_decoder_config_t cfg = {
        .codec = EAF_CODEC_PCM, .sample_rate = 44100, .channels = 2, .bit_depth = 16};
    int32_t pcm[8];
    uint32_t frames = 0;
    eaf_format_t fmt = {0};
    size_t consumed = 0;

    /* Stereo 16-bit, with a frame split across two pushes. */
    CHECK(eaf_decoder_open(&d, &cfg, &out) == EAF_OK);
    const uint8_t stereo[8] = {0x00, 0x80, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x40};
    CHECK(eaf_decoder_push(&d, stereo, 6, &consumed) == EAF_OK);
    CHECK(consumed == 6);
    CHECK(eaf_decoder_pull(&d, pcm, 8, &frames, &fmt) == EAF_OK);
    CHECK(frames == 1);
    CHECK(eaf_format_equal(&fmt, &out));
    CHECK(pcm[0] == eaf_pcm16_to_q31((int16_t)-32768));
    CHECK(pcm[1] == eaf_pcm16_to_q31(32767));
    CHECK(eaf_decoder_push(&d, stereo + 6, 2, &consumed) == EAF_OK);
    CHECK(consumed == 2);
    CHECK(eaf_decoder_pull(&d, pcm, 8, &frames, &fmt) == EAF_OK);
    CHECK(frames == 1);
    CHECK(pcm[0] == 0);
    CHECK(pcm[1] == eaf_pcm16_to_q31(16384));
    eaf_decoder_close(&d);

    /* Mono 16-bit upmixes to stereo. */
    cfg.channels = 1;
    CHECK(eaf_decoder_open(&d, &cfg, &out) == EAF_OK);
    const uint8_t mono[4] = {0xE8, 0x03, 0xD0, 0x07}; /* 1000, 2000 */
    CHECK(eaf_decoder_push(&d, mono, sizeof(mono), &consumed) == EAF_OK);
    CHECK(consumed == sizeof(mono));
    CHECK(eaf_decoder_pull(&d, pcm, 8, &frames, &fmt) == EAF_OK);
    CHECK(frames == 2);
    CHECK(pcm[0] == eaf_pcm16_to_q31(1000) && pcm[1] == eaf_pcm16_to_q31(1000));
    CHECK(pcm[2] == eaf_pcm16_to_q31(2000) && pcm[3] == eaf_pcm16_to_q31(2000));
    eaf_decoder_close(&d);

    /* Stereo 24-bit sign extension. */
    cfg.channels = 2;
    cfg.bit_depth = 24;
    CHECK(eaf_decoder_open(&d, &cfg, &out) == EAF_OK);
    const uint8_t stereo24[6] = {0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x80};
    CHECK(eaf_decoder_push(&d, stereo24, sizeof(stereo24), &consumed) == EAF_OK);
    CHECK(eaf_decoder_pull(&d, pcm, 8, &frames, &fmt) == EAF_OK);
    CHECK(frames == 1);
    CHECK(pcm[0] == eaf_pcm24_to_q31(8388607));
    CHECK(pcm[1] == eaf_pcm24_to_q31(-8388608));
    eaf_decoder_close(&d);

    /* Rate mismatch and a channel change beyond mono-to-stereo are rejected. */
    cfg.bit_depth = 16;
    eaf_format_t other = out;
    other.sample_rate = 48000;
    CHECK(eaf_decoder_open(&d, &cfg, &other) == EAF_UNSUPPORTED);
    return 0;
}

int main(void) {
    if (test_pcm_adapter() || test_dispatch_validation() || test_worker_backpressure() ||
        test_worker_reset_drops_pending() || test_worker_format_mismatch() ||
        test_worker_pull_error_recovers())
        return 1;
    printf("decoder tests passed\n");
    return 0;
}
