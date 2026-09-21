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
    bool configured, running, started, paused;
    void *held;
    uint32_t submitted_blocks;
    void (*error_handler)(const char *operation, int error, uint32_t submitted_blocks);
} state;
void eaf_zephyr_i2s_set_error_handler(void (*handler)(const char *operation, int error,
                                                      uint32_t submitted_blocks)) {
    state.error_handler = handler;
}
static int io_error(const char *operation, int error) {
    if (state.error_handler)
        state.error_handler(operation, error, state.submitted_blocks);
    return EAF_IO;
}
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
        return io_error("device ready", -ENODEV);
    struct i2s_config cfg = {.word_size = 32,
                             .channels = 2,
                             .format = I2S_FMT_DATA_FORMAT_I2S,
                             .options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
                             .frame_clk_freq = fmt->sample_rate,
                             .mem_slab = &tx_blocks,
                             .block_size = frames * 2u * sizeof(int32_t),
                             .timeout = 20};
    int rc = i2s_configure(state.device, I2S_DIR_TX, &cfg);
    if (rc)
        return io_error("configure", rc);
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
    state.paused = false;
    state.submitted_blocks = 0;
    state.running = true;
    state.started = false;
    return EAF_OK;
}
static int acquire(eaf_sink_t *sink, eaf_buffer_t **buffer) {
    (void)sink;
    if (!state.running || state.paused || state.held || !buffer)
        return EAF_STATE;
    int rc = k_mem_slab_alloc(&tx_blocks, &state.held, K_MSEC(20));
    if (rc)
        return io_error("allocate TX block", rc);
    state.buffer.samples = state.held;
    *buffer = &state.buffer;
    return EAF_OK;
}
/* Reclaim every DMA slot after DRAIN. This waits for driver ownership release,
   not a measurement of the amplifier's presentation latency. */
static int drain(void) {
    int trigger_rc = i2s_trigger(state.device, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
    if (trigger_rc)
        return io_error("drain", trigger_rc);
    void *blocks[4];
    unsigned count = 0;
    int64_t deadline = k_uptime_get() + 1000;
    while (count < 4) {
        if (k_mem_slab_alloc(&tx_blocks, &blocks[count], K_TIMEOUT_ABS_MS(deadline)))
            break;
        ++count;
    }
    int rc = count == 4 ? EAF_OK : io_error("drain reclaim", -ETIMEDOUT);
    while (count)
        k_mem_slab_free(&tx_blocks, blocks[--count]);
    if (!rc)
        state.started = false;
    return rc;
}
int eaf_zephyr_i2s_pause(bool paused) {
    if (!state.running || state.held)
        return EAF_STATE;
    if (state.paused == paused)
        return EAF_OK;
    if (paused && state.started) {
        int rc = drain();
        if (rc)
            return rc;
    }
    state.paused = paused;
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
        return io_error("cache flush", cache_rc);
    int wrc = i2s_write(state.device, state.held, bytes);
    if (wrc)
        return io_error("write", wrc);
    state.held = NULL; /* Ownership transferred only on successful write. */
    ++state.submitted_blocks;
    if (!state.started) {
        int trc = i2s_trigger(state.device, I2S_DIR_TX, I2S_TRIGGER_START);
        if (trc)
            return io_error("start", trc);
        state.started = true;
    }
    return (buffer->flags & EAF_FRAME_EOS) ? drain() : EAF_OK;
}
static int stop(eaf_sink_t *sink) {
    (void)sink;
    /* This block never reached the driver. Release it before waiting for the
       entire slab, otherwise drain would wait for our own allocation. */
    if (state.held) {
        k_mem_slab_free(&tx_blocks, state.held);
        state.held = NULL;
    }
    /* The ESP32 backend drops queued blocks but does not reclaim the active DMA
       block on DROP. Let the small hardware queue retire first; the reservoir
       is cancelled by the owner, not drained here. Retain state on failure. */
    if (state.started) {
        int rc = drain();
        if (rc)
            return rc;
    }
    if (state.configured) {
        int rc = i2s_trigger(state.device, I2S_DIR_TX, I2S_TRIGGER_DROP);
        if (rc)
            return io_error("drop", rc);
    }
    state.running = false;
    state.started = false;
    return EAF_OK;
}
static int deinit(eaf_sink_t *sink) {
    int rc = stop(sink);
    if (rc)
        return rc; /* Retain ownership/state if the driver cannot stop. */
    state.configured = false;
    return EAF_OK;
}
static int adjust(eaf_sink_t *sink, int32_t ppm) {
    (void)sink;
    (void)ppm;
    return EAF_UNSUPPORTED;
}
static const struct eaf_sink_ops ops = {init, start, stop, acquire, commit, adjust, deinit};
eaf_sink_t eaf_zephyr_i2s_sink = {&ops, &state};
