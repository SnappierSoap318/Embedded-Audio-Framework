#include <eaf/eaf_dsp.h>
#include <eaf/eaf_sink_null.h>
#include <stdio.h>
#define CAPACITY 16384u
static int32_t storage[CAPACITY * 2u];
static eaf_reservoir_t reservoir;
static eaf_pipeline_t pipeline;
static eaf_null_sink_ctx_t sink_ctx;
static eaf_sink_t sink = {&eaf_null_sink_ops, &sink_ctx};
static eaf_volume_ctx_t headroom = {{1076291389, 1076291389, 1076291389, 1076291389}};
static eaf_crossover_2_1_ctx_t crossover = {.frequency_hz = 80.0};
static eaf_eq_ctx_t eq;
static eaf_volume_ctx_t volume = {{INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX}};
static eaf_node_t attenuation = {"headroom -6 dB", EAF_NODE_STAGE_PRE_PROCESS, &eaf_volume_ops,
                                 &headroom};
static eaf_node_t cross = {"LR4 80 Hz", EAF_NODE_STAGE_PRE_PROCESS, &eaf_crossover_2_1_ops,
                           &crossover};
static eaf_node_t equalizer = {"EQ", EAF_NODE_STAGE_DSP, &eaf_biquad_eq_ops, &eq};
static eaf_node_t master = {"volume", EAF_NODE_STAGE_POST_PROCESS, &eaf_volume_ops, &volume};
static eaf_node_t *const nodes[] = {&attenuation, &cross, &equalizer, &master};
static eaf_atomic_u32_t run_gate, quit;
static eaf_sem_t wake;
static void wake_producer(void *ctx) {
    hal_sem_give(ctx);
}
static void produce(void *ctx) {
    (void)ctx;
    int32_t block[256u * 2u];
    uint32_t phase = 0;
    while (!hal_atomic_get(&run_gate) && !hal_atomic_get(&quit))
        hal_sleep_ms(1);
    while (!hal_atomic_get(&quit)) {
        if (eaf_reservoir_backpressure(&reservoir)) {
            while (eaf_reservoir_level(&reservoir) >= reservoir.backpressure_low &&
                   !hal_atomic_get(&quit))
                (void)hal_sem_take(&wake, 20);
        }
        for (size_t i = 0; i < 256u; ++i) {
            /* Low-amplitude 375 Hz triangle, no runtime floating-point work. */
            int32_t sample = (int32_t)(phase < 64u ? phase : 127u - phase) * 8388608 - 268435456;
            block[i * 2u] = sample;
            block[i * 2u + 1u] = sample;
            phase = (phase + 1u) % 128u;
        }
        uint32_t offset = 0;
        while (offset < 256u && !hal_atomic_get(&quit)) {
            uint32_t written =
                eaf_reservoir_write(&reservoir, block + (size_t)offset * 2u, 256u - offset);
            offset += written;
            if (!written)
                (void)hal_sem_take(&wake, 20);
        }
    }
}
int main(void) {
    eaf_format_t fmt = {48000, 2, 3};
    eaf_pipeline_config_t config = {&reservoir, nodes, 4, &sink};
    eaf_thread_t decoder = {0};
    if (eaf_reservoir_init(&reservoir, storage, CAPACITY, fmt, 8192) || hal_sem_init(&wake) ||
        eaf_pipeline_init(&pipeline, &config) || eaf_pipeline_configure(&pipeline, 128))
        return 1;
    reservoir.wake_producer = wake_producer;
    reservoir.wake_ctx = &wake;
    if (hal_thread_create(&decoder, produce, NULL))
        return 1;
    int rc = eaf_pipeline_start(&pipeline);
    if (!rc)
        hal_atomic_set(&run_gate, 1);
    for (unsigned i = 0; !rc && i < 375u; ++i)
        rc = eaf_pipeline_process(&pipeline);
    hal_atomic_set(&quit, 1);
    hal_sem_give(&wake);
    if (hal_thread_join(&decoder))
        rc = EAF_IO;
    if (pipeline.state == EAF_RUNNING && eaf_pipeline_stop(&pipeline))
        rc = EAF_IO;
    printf("Processed %llu frames, output channels=%u, underruns=%u\n",
           (unsigned long long)sink_ctx.frames_committed,
           (unsigned)pipeline.output_format.num_channels, reservoir.underruns);
    if (eaf_pipeline_deinit(&pipeline))
        rc = EAF_IO;
    hal_sem_deinit(&wake);
    return rc ? 1 : 0;
}
