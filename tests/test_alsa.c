#include "check.h"
#include <eaf/eaf_dsp.h>
#include <eaf/eaf_sink_alsa.h>
static eaf_alsa_sink_ctx_t ctx;
int main(int argc, char **argv) {
    CHECK(argc == 2);
    ctx.device = argv[1];
    eaf_sink_t sink = {&eaf_alsa_sink_ops, &ctx};
    eaf_format_t fmt = {48000, 2, 3};
    CHECK(sink.ops->init(&sink, &fmt, 128) == 0);
    CHECK(sink.ops->start(&sink) == 0);
    CHECK(sink.ops->start(&sink) == EAF_STATE);
    eaf_volume_ctx_t volume = {{1073741824, 0, INT32_MAX, INT32_MAX}};
    eaf_node_t node = {"volume", EAF_NODE_STAGE_POST_PROCESS, &eaf_volume_ops, &volume};
    eaf_format_t out;
    CHECK(node.ops->init(&node, &fmt, &out) == 0);
    for (unsigned block = 0; block < 2; ++block) {
        eaf_buffer_t *buf;
        CHECK(sink.ops->acquire_buf(&sink, &buf) == 0);
        CHECK(sink.ops->acquire_buf(&sink, &buf) == EAF_STATE);
        for (size_t i = 0; i < 128; ++i) {
            buf->samples[i * 2] = 1073741824;
            buf->samples[i * 2 + 1] = -1073741824;
        }
        CHECK(node.ops->process(&node, buf) == 0);
        if (block == 1)
            buf->flags |= EAF_FRAME_EOS;
        CHECK(sink.ops->commit_buf(&sink, buf) == 0);
        CHECK(sink.ops->commit_buf(&sink, buf) == EAF_STATE);
        if (!block) {
            CHECK(eaf_alsa_pause(&ctx, true) == 0);
            CHECK(sink.ops->acquire_buf(&sink, &buf) == EAF_STATE);
            CHECK(eaf_alsa_pause(&ctx, false) == 0);
        }
    }
    CHECK(ctx.frames_written == 256 && ctx.xruns == 0);
    CHECK(eaf_alsa_drain(&ctx) == 0);
    CHECK(sink.ops->stop(&sink) == 0);
    CHECK(sink.ops->start(&sink) == 0);
    CHECK(sink.ops->stop(&sink) == 0);
    sink.ops->deinit(&sink);
    CHECK(!ctx.pcm);
    return 0;
}
