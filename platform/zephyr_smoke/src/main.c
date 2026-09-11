#include <eaf/eaf_bt.h>
#include <eaf/eaf_core.h>
#include <eaf/eaf_lms.h>
#include <eaf/eaf_sink_i2s.h>
#include <eaf/eaf_sink_null.h>
#include <zephyr/kernel.h>
static eaf_reservoir_t reservoir;
static int32_t storage[256];
static eaf_null_sink_ctx_t sink_ctx;
static eaf_pipeline_t pipeline;
static eaf_sem_t ready;
static eaf_bt_ingress_t bt;
static eaf_lms_parser_t lms;
static int packets;
extern unsigned eaf_mock_commits;
extern int32_t eaf_mock_last;
extern bool eaf_mock_fail_write;
extern int eaf_sbc_smoke(void);
extern int eaf_bt_smoke(void);
static int packet(void *ctx, const uint8_t *data, size_t length) {
    (void)ctx;
    (void)data;
    if (length != 4)
        return EAF_INVALID;
    ++packets;
    return EAF_OK;
}
static void producer(void *ctx) {
    (void)ctx;
    int32_t samples[64];
    for (size_t i = 0; i < 64; ++i)
        samples[i] = (int32_t)i;
    (void)eaf_reservoir_write(&reservoir, samples, 32);
    eaf_reservoir_finish(&reservoir);
    hal_sem_give(&ready);
}
int main(void) {
    eaf_format_t fmt = {48000, 2, 3};
    eaf_sink_t sink = {&eaf_null_sink_ops, &sink_ctx};
    eaf_pipeline_config_t config = {&reservoir, NULL, 0, &sink};
    eaf_thread_t worker = {0};
    int rc = eaf_reservoir_init(&reservoir, storage, 128, fmt, 64);
    if (!rc)
        rc = hal_sem_init(&ready);
    if (!rc)
        rc = eaf_pipeline_init(&pipeline, &config);
    if (!rc)
        rc = eaf_pipeline_configure(&pipeline, 32);
    if (!rc)
        rc = eaf_pipeline_start(&pipeline);
    /* Static Zephyr pool, no heap allocation even when creating this worker. */
    if (!rc)
        rc = hal_thread_create(&worker, producer, NULL);
    if (!rc)
        rc = hal_sem_take(&ready, 1000);
    if (!rc)
        rc = hal_thread_join(&worker);
    if (!rc && eaf_pipeline_process(&pipeline) != EAF_EOF)
        rc = EAF_IO;
    if (!rc && (reservoir.frames_read != 32 || sink_ctx.samples[0][63] != 63))
        rc = EAF_IO;
    if (!rc)
        rc = eaf_pipeline_stop(&pipeline);
    if (!rc)
        rc = eaf_pipeline_deinit(&pipeline);
    hal_sem_deinit(&ready);
    const uint8_t frame[] = {0, 4, 'a', 'u', 'd', 'e'};
    eaf_lms_parser_init(&lms, packet, NULL);
    if (!rc)
        rc = eaf_lms_feed(&lms, frame, sizeof(frame));
    if (!rc && packets != 1)
        rc = EAF_IO;
    eaf_bt_ingress_init(&bt);
    const uint8_t sbc[] = {1, 0x9c, 0, 0, 0};
    static eaf_bt_packet_t out;
    if (!rc)
        rc = eaf_bt_sbc_receive(&bt, 1, 0, sbc, sizeof(sbc));
    if (!rc && !eaf_bt_ingress_pop(&bt, &out))
        rc = EAF_IO;
    if (!rc)
        rc = eaf_zephyr_i2s_bind("eaf_mock_i2s");
    eaf_sink_t *tx = &eaf_zephyr_i2s_sink;
    if (!rc)
        rc = tx->ops->init(tx, &fmt, 32);
    /* More than four restarts verifies that STOP releases failed-write buffers. */
    for (unsigned cycle = 0; !rc && cycle < 10; ++cycle) {
        rc = tx->ops->start(tx);
        eaf_buffer_t *b = NULL;
        if (!rc)
            rc = tx->ops->acquire_buf(tx, &b);
        if (!rc) {
            for (size_t i = 0; i < 64; ++i)
                b->samples[i] = (int32_t)i;
            eaf_mock_fail_write = (cycle & 1u) != 0;
            int result = tx->ops->commit_buf(tx, b);
            if (result != (eaf_mock_fail_write ? EAF_IO : EAF_OK))
                rc = EAF_IO;
        }
        if (!rc)
            rc = tx->ops->stop(tx);
    }
    if (!rc && (eaf_mock_commits != 5 || eaf_mock_last != 63))
        rc = EAF_IO;
    tx->ops->deinit(tx);
    if (!rc)
        rc = eaf_sbc_smoke();
    if (rc == EAF_OK)
        rc = eaf_bt_smoke();
    printk("EAF smoke %s (%d)\n", rc ? "FAIL" : "PASS", rc);
    return rc;
}
