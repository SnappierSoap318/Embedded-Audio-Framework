#include <eaf/eaf_sink_i2s_core.h>
#include <stdint.h>

int eaf_i2s_core_io_error(eaf_i2s_core_t *core, const char *operation, int error) {
    if (core->error_handler)
        core->error_handler(operation, error, core->submitted_blocks);
    return EAF_IO;
}

void eaf_i2s_core_set_error_handler(eaf_i2s_core_t *core,
                                    void (*handler)(const char *operation, int error,
                                                    uint32_t submitted_blocks)) {
    core->error_handler = handler;
}

int eaf_i2s_core_drain(eaf_i2s_core_t *core) {
    if (!core->started)
        return EAF_OK;
    int rc = core->backend->drain(core);
    if (rc)
        return eaf_i2s_core_io_error(core, "drain", rc);
    if (core->backend->disable) {
        rc = core->backend->disable(core);
        if (rc)
            return eaf_i2s_core_io_error(core, "disable", rc);
    }
    core->started = false;
    return EAF_OK;
}

int eaf_i2s_core_pause(eaf_i2s_core_t *core, bool paused) {
    if (!core->running || core->held)
        return EAF_STATE;
    if (core->paused == paused)
        return EAF_OK;
    if (paused && core->started) {
        int rc = eaf_i2s_core_drain(core);
        if (rc)
            return rc;
    }
    core->paused = paused;
    return EAF_OK;
}

static int core_init(eaf_sink_t *sink, const eaf_format_t *fmt, size_t frames) {
    eaf_i2s_core_t *core = sink->driver_data;
    if (!core || !core->backend || core->configured || core->running)
        return EAF_STATE;
    if (!eaf_format_valid(fmt) || fmt->num_channels != 2 || fmt->channel_mask != 3 || !frames)
        return EAF_INVALID;
    if (frames > EAF_I2S_CORE_MAX_FRAMES)
        frames = EAF_I2S_CORE_MAX_FRAMES;
    int rc = core->backend->configure(core, fmt, frames);
    if (rc)
        return rc;
    core->frames = (uint32_t)frames;
    core->buffer = (eaf_buffer_t){.samples = NULL,
                                  .frame_count = (uint32_t)frames,
                                  .capacity_frames = (uint32_t)frames,
                                  .capacity_samples = frames * 2u,
                                  .format = *fmt,
                                  .flags = 0};
    core->dma = NULL;
    core->held = false;
    core->configured = true;
    return EAF_OK;
}

static int core_start(eaf_sink_t *sink) {
    eaf_i2s_core_t *core = sink->driver_data;
    if (!core->configured || core->running)
        return EAF_STATE;
    /* Zephyr requires a queued block before the hardware START trigger. */
    core->paused = false;
    core->submitted_blocks = 0;
    core->running = true;
    core->started = false;
    core->held = false;
    return EAF_OK;
}

static int core_acquire(eaf_sink_t *sink, eaf_buffer_t **buffer) {
    eaf_i2s_core_t *core = sink->driver_data;
    if (!core->running || core->paused || core->held || !buffer)
        return EAF_STATE;
    void *block = core->backend->dma_alloc(core, (size_t)core->frames * 2u * sizeof(int32_t));
    if (!block)
        return eaf_i2s_core_io_error(core, "allocate", EAF_IO);
    core->dma = block;
    core->held = true;
    core->buffer.samples = block;
    core->buffer.frame_count = core->frames;
    core->buffer.flags = 0;
    *buffer = &core->buffer;
    return EAF_OK;
}

static int core_commit(eaf_sink_t *sink, eaf_buffer_t *buffer) {
    eaf_i2s_core_t *core = sink->driver_data;
    if (!core->running || !core->held || buffer != &core->buffer)
        return EAF_STATE;
    if (!core->started && core->backend->enable_before_write) {
        int enable_rc = core->backend->enable(core);
        if (enable_rc)
            return eaf_i2s_core_io_error(core, "enable", enable_rc);
        core->started = true;
    }
    size_t bytes = (size_t)buffer->frame_count * 2u * sizeof(int32_t);
    size_t written = 0;
    int rc = core->backend->write(core, core->dma, bytes, &written);
    if (rc || written != bytes)
        return eaf_i2s_core_io_error(core, "write", rc ? rc : EAF_IO);
    core->held = false; /* Ownership transferred only on a complete write. */
    core->dma = NULL;
    ++core->submitted_blocks;
    if (!core->started) {
        int start_rc = core->backend->start(core);
        if (start_rc)
            return eaf_i2s_core_io_error(core, "start", start_rc);
        core->started = true;
    }
    return (buffer->flags & EAF_FRAME_EOS) ? eaf_i2s_core_drain(core) : EAF_OK;
}

static int core_stop(eaf_sink_t *sink) {
    eaf_i2s_core_t *core = sink->driver_data;
    /* The uncommitted block never reached the driver: reclaim it before waiting
       for the DMA queue, otherwise drain would wait for our own allocation. */
    if (core->held) {
        core->backend->dma_free(core, core->dma);
        core->dma = NULL;
        core->held = false;
    }
    if (core->started) {
        int rc = eaf_i2s_core_drain(core);
        if (rc)
            return rc; /* Retain ownership/state if the driver cannot stop. */
    }
    if (core->backend->drop) {
        int rc = core->backend->drop(core);
        if (rc)
            return eaf_i2s_core_io_error(core, "drop", rc);
    }
    core->running = false;
    core->paused = false;
    return EAF_OK;
}

static int core_deinit(eaf_sink_t *sink) {
    eaf_i2s_core_t *core = sink->driver_data;
    int rc = core_stop(sink);
    if (rc)
        return rc; /* Retain ownership/state if the driver cannot stop. */
    if (core->backend->delete_channel) {
        int delete_rc = core->backend->delete_channel(core);
        if (delete_rc)
            return eaf_i2s_core_io_error(core, "delete", delete_rc);
    }
    core->configured = false;
    return EAF_OK;
}

static int core_adjust(eaf_sink_t *sink, int32_t ppm) {
    eaf_i2s_core_t *core = sink->driver_data;
    if (!core->backend->tune_ppm)
        return EAF_UNSUPPORTED;
    return core->backend->tune_ppm(core, ppm);
}

const struct eaf_sink_ops eaf_i2s_core_sink_ops = {
    core_init, core_start, core_stop, core_acquire, core_commit, core_adjust, core_deinit};
