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
static int scheduling_result;
extern unsigned eaf_mock_commits;
extern int32_t eaf_mock_last;
extern bool eaf_mock_fail_write, eaf_mock_fail_start, eaf_mock_fail_drop, eaf_mock_fail_drain;
extern bool eaf_mock_require_drain;
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
static void scheduling_worker(void *ctx) {
    const int *priority = ctx;
    scheduling_result = k_thread_priority_get(k_current_get()) == *priority ? EAF_OK : EAF_IO;
    hal_sem_give(&ready);
}
static int scheduling_smoke(void) {
    if (hal_sem_take(&ready, 0) != EAF_TIMEOUT)
        return EAF_IO;
    uint64_t before = hal_monotonic_time_us();
    if (hal_sem_take(&ready, 10) != EAF_TIMEOUT || hal_monotonic_time_us() - before < 10000u)
        return EAF_IO;
    hal_sem_give(&ready);
    hal_sem_give(&ready);
    if (hal_sem_take(&ready, 0) || hal_sem_take(&ready, 0) != EAF_TIMEOUT)
        return EAF_IO;
    for (unsigned i = 0; i < 8; ++i) {
        eaf_thread_t thread = {0};
        eaf_thread_options_t options = {i & 1u ? EAF_THREAD_AUDIO : EAF_THREAD_DECODER, -1, false};
        int priority = i & 1u ? CONFIG_EAF_AUDIO_PRIORITY : CONFIG_EAF_DECODER_PRIORITY;
        int rc = hal_thread_create_with_options(&thread, scheduling_worker, &priority, &options);
        if (rc)
            return rc;
        int waited = hal_sem_take(&ready, 1000);
        int joined = hal_thread_join(&thread);
        if (waited || joined || scheduling_result)
            return EAF_IO;
    }
    eaf_thread_t thread = {0};
    eaf_thread_options_t options = {EAF_THREAD_AUDIO, 0, false};
    int priority = CONFIG_EAF_AUDIO_PRIORITY;
    int rc = hal_thread_create_with_options(&thread, scheduling_worker, &priority, &options);
    if (rc == EAF_UNSUPPORTED) {
        /* Affinity is not built in: the request must leave no thread handle. */
        if (thread.impl)
            return EAF_IO;
    } else if (rc || hal_sem_take(&ready, 1000) || hal_thread_join(&thread) || scheduling_result) {
        return EAF_IO;
    }
    return EAF_OK;
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
        rc = scheduling_smoke();
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
            eaf_mock_fail_start = !eaf_mock_fail_write;
            int result = tx->ops->commit_buf(tx, b);
            if (result != EAF_IO)
                rc = EAF_IO;
        }
        eaf_mock_fail_drop = true;
        if (!rc && (tx->ops->stop(tx) != EAF_IO || tx->ops->deinit(tx) != EAF_IO ||
                    tx->ops->start(tx) != EAF_STATE))
            rc = EAF_IO;
        eaf_mock_fail_drop = false;
        if (!rc)
            rc = tx->ops->stop(tx);
    }
    if (!rc && (eaf_mock_commits != 5 || eaf_mock_last != 63))
        rc = EAF_IO;
    eaf_mock_fail_start = eaf_mock_fail_write = false;
    for (unsigned cycle = 0; !rc && cycle < 2; ++cycle) {
        rc = tx->ops->start(tx);
        eaf_buffer_t *b = NULL;
        if (!rc)
            rc = tx->ops->acquire_buf(tx, &b);
        if (!rc) {
            b->flags = EAF_FRAME_EOS;
            eaf_mock_fail_drain = cycle == 0;
            if (tx->ops->commit_buf(tx, b) != (eaf_mock_fail_drain ? EAF_IO : EAF_OK))
                rc = EAF_IO;
        }
        if (!rc && eaf_mock_fail_drain && tx->ops->stop(tx) != EAF_IO)
            rc = EAF_IO;
        eaf_mock_fail_drain = false; /* Retry cleanup once the backend recovers. */
        if (!rc)
            rc = tx->ops->stop(tx);
    }
    eaf_mock_fail_drain = false;
    if (!rc)
        rc = tx->ops->start(tx);
    if (!rc)
        rc = eaf_zephyr_i2s_pause(true);
    eaf_buffer_t *paused_buffer = NULL;
    if (!rc && tx->ops->acquire_buf(tx, &paused_buffer) != EAF_STATE)
        rc = EAF_IO;
    if (!rc)
        rc = eaf_zephyr_i2s_pause(false);
    if (!rc)
        rc = tx->ops->acquire_buf(tx, &paused_buffer);
    if (!rc) {
        paused_buffer->flags = 0;
        rc = tx->ops->commit_buf(tx, paused_buffer);
    }
    eaf_mock_fail_drain = true;
    if (!rc && eaf_zephyr_i2s_pause(true) != EAF_IO)
        rc = EAF_IO;
    eaf_mock_fail_drain = false;
    if (!rc)
        rc = eaf_zephyr_i2s_pause(true);
    if (!rc)
        rc = eaf_zephyr_i2s_pause(false);
    if (!rc)
        rc = tx->ops->acquire_buf(tx, &paused_buffer);
    if (!rc) {
        paused_buffer->flags = 0;
        rc = tx->ops->commit_buf(tx, paused_buffer);
    }
    if (!rc)
        rc = tx->ops->stop(tx);
    /* Track cancellation must retire in-flight DMA before DROP, including when
       the producer owns another uncommitted block. Repeat beyond pool capacity. */
    eaf_mock_require_drain = true;
    for (unsigned cycle = 0; !rc && cycle < 10; ++cycle) {
        rc = tx->ops->start(tx);
        eaf_buffer_t *b = NULL;
        if (!rc)
            rc = tx->ops->acquire_buf(tx, &b);
        if (!rc) {
            b->flags = 0;
            rc = tx->ops->commit_buf(tx, b);
        }
        if (!rc)
            rc = tx->ops->acquire_buf(tx, &b);
        if (!rc)
            rc = tx->ops->stop(tx);
    }
    eaf_mock_require_drain = false;
    int cleanup = tx->ops->deinit(tx);
    if (!rc)
        rc = cleanup;
    if (!rc)
        rc = eaf_sbc_smoke();
    if (rc == EAF_OK)
        rc = eaf_bt_smoke();
    printk("EAF smoke %s (%d)\n", rc ? "FAIL" : "PASS", rc);
    return rc;
}
