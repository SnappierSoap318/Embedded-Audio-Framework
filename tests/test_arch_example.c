/* Runnable architecture v0.5 example: one finite stereo source, one owner. */
#include "check.h"
#include <eaf/eaf_dsp.h>
#include <eaf/eaf_sink_null.h>
static int32_t storage[64u * 2u];
static eaf_reservoir_t reservoir;
static eaf_pipeline_t pipeline;
static eaf_null_sink_ctx_t output;
static eaf_sink_t sink = {&eaf_null_sink_ops, &output};
static eaf_volume_ctx_t gain = {{INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX}};
static eaf_node_t master = {"master", EAF_NODE_STAGE_POST_PROCESS, &eaf_volume_ops, &gain};
static eaf_node_t *const nodes[] = {&master};
int main(void) {
    const eaf_format_t stereo = {48000, 2, EAF_CH_FRONT_LEFT | EAF_CH_FRONT_RIGHT};
    const eaf_pipeline_config_t config = {&reservoir, nodes, 1, &sink};
    const int32_t pcm[] = {1073741824, -1073741824};
    CHECK(!eaf_reservoir_init(&reservoir, storage, 64, stereo, 32));
    CHECK(!eaf_pipeline_init(&pipeline, &config));
    CHECK(!eaf_pipeline_configure(&pipeline, 16));
    CHECK(!eaf_pipeline_start(&pipeline)); /* Resets reservoir before publication. */
    CHECK(eaf_reservoir_write(&reservoir, pcm, 1) == 1);
    eaf_reservoir_finish(&reservoir);
    CHECK(eaf_pipeline_process(&pipeline) == EAF_EOF);
    CHECK(reservoir.frames_read == 1 && reservoir.underruns == 0);
    CHECK(output.frames_committed == 16);              /* Final block has 15 padding frames. */
    CHECK(eaf_pipeline_process(&pipeline) == EAF_EOF); /* No extra output. */
    CHECK(output.frames_committed == 16);
    CHECK(!eaf_pipeline_stop(&pipeline));
    CHECK(!eaf_pipeline_deinit(&pipeline));
    return 0;
}
