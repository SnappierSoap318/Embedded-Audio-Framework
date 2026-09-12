#include <eaf/eaf_dsp.h>
#include <eaf/eaf_sink_null.h>
#include <zephyr/kernel.h>
BUILD_ASSERT(!IS_ENABLED(CONFIG_ESP_SPIRAM), "Boot test must use internal RAM");
BUILD_ASSERT(!IS_ENABLED(CONFIG_I2S), "Wire and validate DACs in a later application");
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT) && !IS_ENABLED(CONFIG_NETWORKING),
             "Boot test must keep radios disabled");
#define FRAMES 4096u
static int32_t storage[FRAMES * 2u];
static eaf_reservoir_t reservoir;
static eaf_pipeline_t pipeline;
static eaf_null_sink_ctx_t output;
static eaf_sink_t sink = {&eaf_null_sink_ops, &output};
static eaf_volume_ctx_t gain = {{INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX}};
static eaf_node_t volume = {"volume", EAF_NODE_STAGE_POST_PROCESS, &eaf_volume_ops, &gain};
static eaf_node_t *const nodes[] = {&volume};
static eaf_atomic_u32_t gate, quit;
static void produce(void *arg) {
    (void)arg;
    int32_t pcm[128u * 2u];
    for (size_t i = 0; i < 128u; ++i) {
        pcm[2u * i] = 1073741824;
        pcm[2u * i + 1u] = -1073741824;
    }
    while (!hal_atomic_get(&gate) && !hal_atomic_get(&quit))
        hal_sleep_ms(1);
    uint32_t sent = 0;
    while (sent < FRAMES && !hal_atomic_get(&quit)) {
        uint32_t n = eaf_reservoir_write(&reservoir, pcm, 128);
        sent += n;
        if (!n)
            hal_sleep_ms(1);
    }
    if (!hal_atomic_get(&quit))
        eaf_reservoir_finish(&reservoir);
}
int main(void) {
    printk("EAF WROOM boot: internal RAM, null output, no DAC pins driven\n");
    printk("Reservoir=%u bytes (%u frames); sink=%u bytes\n", (unsigned)sizeof(storage), FRAMES,
           (unsigned)sizeof(output));
    eaf_format_t fmt = {48000, 2, EAF_CH_FRONT_LEFT | EAF_CH_FRONT_RIGHT};
    eaf_pipeline_config_t config = {&reservoir, nodes, 1, &sink};
    eaf_thread_t decoder = {0};
    int rc = eaf_reservoir_init(&reservoir, storage, FRAMES, fmt, 1024);
    if (!rc)
        rc = eaf_pipeline_init(&pipeline, &config);
    if (!rc)
        rc = eaf_pipeline_configure(&pipeline, 128);
    if (!rc)
        rc = hal_thread_create(&decoder, produce, NULL);
    if (!rc)
        rc = eaf_pipeline_start(&pipeline);
    if (!rc)
        hal_atomic_set(&gate, 1);
    uint64_t deadline = hal_monotonic_time_us() + 2000000u;
    while (!rc && hal_monotonic_time_us() < deadline)
        rc = eaf_pipeline_process(&pipeline);
    if (rc == EAF_EOF)
        rc = reservoir.frames_read == FRAMES && !reservoir.underruns ? EAF_OK : EAF_IO;
    else if (!rc)
        rc = EAF_TIMEOUT;
    hal_atomic_set(&quit, 1);
    if (decoder.impl && hal_thread_join(&decoder)) {
        printk("EAF boot FAIL: decoder join; retaining graph\n");
        return 1;
    }
    if ((pipeline.state == EAF_RUNNING || pipeline.state == EAF_RECOVERY) &&
        eaf_pipeline_stop(&pipeline)) {
        printk("EAF boot FAIL: output stop; retaining graph\n");
        return 1;
    }
    int cleanup = eaf_pipeline_deinit(&pipeline);
    if (!rc)
        rc = cleanup;
    printk("EAF boot %s (%d): source=%u frames, underruns=%u\n", rc ? "FAIL" : "PASS", rc,
           (unsigned)reservoir.frames_read, reservoir.underruns);
    for (;;) {
        printk("EAF alive: uptime=%u ms, test=%s\n", (unsigned)k_uptime_get(),
               rc ? "FAIL" : "PASS");
        k_sleep(K_SECONDS(5));
    }
    return 0;
}
