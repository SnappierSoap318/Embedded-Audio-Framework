#include <eaf/eaf_sink_i2s.h>
#include <errno.h>
#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#define SLOT_BYTES ROUND_UP((size_t)CONFIG_EAF_I2S_MAX_FRAMES * 2u * sizeof(int32_t), 32u)
/* Driver returns completed blocks to this fixed pool. No heap or growing pool. */
K_MEM_SLAB_DEFINE_STATIC(tx_blocks, SLOT_BYTES, 4, 32);
static struct {
    const char *name;
    const struct device *device;
    eaf_buffer_t buffer;
    bool configured, running, started;
    void *held;
} state;
int eaf_zephyr_i2s_bind(const char *name) {
    if (!name)
        return EAF_INVALID;
    if (state.configured || state.running)
        return EAF_STATE;
    state.name = name;
    return EAF_OK;
}
static int init(eaf_sink_t *sink, const eaf_format_t *fmt, size_t frames) {
    (void)sink;
    if (state.configured || state.running)
        return EAF_STATE;
    if (!state.name || !eaf_format_valid(fmt) || fmt->num_channels != 2 || fmt->channel_mask != 3 ||
        !frames || frames > CONFIG_EAF_I2S_MAX_FRAMES)
        return EAF_INVALID;
    state.device = device_get_binding(state.name);
    if (!state.device || !device_is_ready(state.device))
        return EAF_IO;
    struct i2s_config cfg = {.word_size = 32,
                             .channels = 2,
                             .format = I2S_FMT_DATA_FORMAT_I2S,
                             .options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
                             .frame_clk_freq = fmt->sample_rate,
                             .mem_slab = &tx_blocks,
                             .block_size = frames * 2u * sizeof(int32_t),
                             .timeout = 20};
    if (i2s_configure(state.device, I2S_DIR_TX, &cfg))
        return EAF_IO;
    state.buffer = (eaf_buffer_t){.frame_count = (uint32_t)frames,
                                  .capacity_frames = (uint32_t)frames,
                                  .capacity_samples = frames * 2u,
                                  .format = *fmt};
    state.configured = true;
    return EAF_OK;
}
static int start(eaf_sink_t *sink) {
    (void)sink;
    if (!state.configured || state.running)
        return EAF_STATE;
    /* Zephyr requires a queued block before the hardware START trigger. */
    state.running = true;
    state.started = false;
    return EAF_OK;
}
static int acquire(eaf_sink_t *sink, eaf_buffer_t **buffer) {
    (void)sink;
    if (!state.running || state.held || !buffer)
        return EAF_STATE;
    if (k_mem_slab_alloc(&tx_blocks, &state.held, K_MSEC(20)))
        return EAF_IO;
    state.buffer.samples = state.held;
    *buffer = &state.buffer;
    return EAF_OK;
}
static int commit(eaf_sink_t *sink, eaf_buffer_t *buffer) {
    (void)sink;
    if (!state.running || !state.held || buffer != &state.buffer || buffer->samples != state.held ||
        buffer->frame_count != buffer->capacity_frames)
        return EAF_STATE;
    size_t bytes = (size_t)buffer->frame_count * 2u * sizeof(int32_t);
    int cache_rc = sys_cache_data_flush_range(state.held, bytes);
    if (cache_rc && cache_rc != -ENOTSUP)
        return EAF_IO;
    if (i2s_write(state.device, state.held, bytes))
        return EAF_IO;
    state.held = NULL; /* Ownership transferred only on successful write. */
    if (!state.started) {
        if (i2s_trigger(state.device, I2S_DIR_TX, I2S_TRIGGER_START))
            return EAF_IO;
        state.started = true;
    }
    return EAF_OK;
}
static int stop(eaf_sink_t *sink) {
    (void)sink;
    if (state.configured && i2s_trigger(state.device, I2S_DIR_TX, I2S_TRIGGER_DROP))
        return EAF_IO;
    if (state.held) {
        k_mem_slab_free(&tx_blocks, state.held);
        state.held = NULL;
    }
    state.running = false;
    state.started = false;
    return EAF_OK;
}
static void deinit(eaf_sink_t *sink) {
    if (stop(sink))
        return; /* Retain ownership/state if the driver cannot stop. */
    state.configured = false;
}
static int adjust(eaf_sink_t *sink, int32_t ppm) {
    (void)sink;
    (void)ppm;
    return EAF_UNSUPPORTED;
}
static const struct eaf_sink_ops ops = {init, start, stop, acquire, commit, adjust, deinit};
eaf_sink_t eaf_zephyr_i2s_sink = {&ops, &state};
