#include <eaf/eaf_core.h>
#include <string.h>
static void release(eaf_pipeline_t *p) {
    if (p->sink_initialized)
        p->config.sink->ops->deinit(p->config.sink);
    p->sink_initialized = false;
    while (p->initialized_nodes) {
        eaf_node_t *n = p->config.nodes[--p->initialized_nodes];
        n->ops->deinit(n);
    }
}
int eaf_pipeline_init(eaf_pipeline_t *p, const eaf_pipeline_config_t *config) {
    if (!p || !config || !config->reservoir || !config->reservoir->storage || !config->sink ||
        !config->sink->ops || (config->node_count && !config->nodes))
        return EAF_INVALID;
    if (p->state != EAF_UNINITIALIZED)
        return EAF_STATE;
    const struct eaf_sink_ops *s = config->sink->ops;
    if (!s->init || !s->start || !s->stop || !s->acquire_buf || !s->commit_buf || !s->deinit)
        return EAF_INVALID;
    for (size_t i = 0; i < config->node_count; ++i) {
        const eaf_node_t *n = config->nodes[i];
        if (!n || !n->ops || !n->ops->init || !n->ops->process || !n->ops->reset ||
            !n->ops->deinit || n->stage > EAF_NODE_STAGE_POST_PROCESS ||
            (i && config->nodes[i - 1u]->stage > n->stage))
            return EAF_INVALID;
    }
    p->config = *config;
    p->state = EAF_INITIALIZED;
    return EAF_OK;
}
int eaf_pipeline_configure(eaf_pipeline_t *p, uint32_t block_frames) {
    if (!p)
        return EAF_INVALID;
    if (p->state != EAF_INITIALIZED && p->state != EAF_STOPPED && p->state != EAF_CONFIGURED)
        return EAF_STATE;
    if (!block_frames || block_frames > p->config.reservoir->capacity)
        return EAF_INVALID;
    release(p);
    p->state = EAF_INITIALIZED;
    eaf_format_t fmt = p->config.reservoir->format;
    for (size_t i = 0; i < p->config.node_count; ++i) {
        eaf_node_t *n = p->config.nodes[i];
        eaf_format_t next = fmt;
        int rc = n->ops->init(n, &fmt, &next);
        if (rc) {
            n->ops->deinit(n);
            release(p);
            return rc;
        }
        ++p->initialized_nodes;
        if (!eaf_format_valid(&next) || next.sample_rate != fmt.sample_rate ||
            next.num_channels < fmt.num_channels) {
            release(p);
            return EAF_INVALID;
        }
        fmt = next;
    }
    int rc = p->config.sink->ops->init(p->config.sink, &fmt, block_frames);
    if (rc) {
        p->config.sink->ops->deinit(p->config.sink);
        release(p);
        return rc;
    }
    p->sink_initialized = true;
    p->output_format = fmt;
    p->block_frames = block_frames;
    p->state = EAF_CONFIGURED;
    return EAF_OK;
}
int eaf_pipeline_start(eaf_pipeline_t *p) {
    if (!p)
        return EAF_INVALID;
    if (p->state != EAF_CONFIGURED && p->state != EAF_STOPPED)
        return EAF_STATE;
    eaf_reservoir_reset(p->config.reservoir);
    p->completed = false;
    int rc = p->config.sink->ops->start(p->config.sink);
    if (!rc)
        p->state = EAF_RUNNING;
    return rc;
}
int eaf_pipeline_process(eaf_pipeline_t *p) {
    if (!p)
        return EAF_INVALID;
    if (p->state != EAF_RUNNING)
        return EAF_STATE;
    if (p->completed)
        return EAF_EOF;
    eaf_sink_t *sink = p->config.sink;
    eaf_buffer_t *buf = NULL;
    int rc = sink->ops->acquire_buf(sink, &buf);
    if (rc)
        return rc;
    if (!buf || !buf->samples || buf->capacity_frames < p->block_frames ||
        buf->capacity_samples / p->output_format.num_channels < p->block_frames)
        return EAF_INVALID;
    eaf_buffer_t owned = *buf;
    buf->frame_count = p->block_frames;
    rc = eaf_reservoir_pull(p->config.reservoir, buf);
    bool eos = !rc && (buf->flags & EAF_FRAME_EOS) != 0;
    for (size_t i = 0; !rc && i < p->config.node_count; ++i) {
        uint8_t channels = buf->format.num_channels;
        rc = p->config.nodes[i]->ops->process(p->config.nodes[i], buf);
        /* Nodes may change samples and expand format, never replace storage.
           Validate before another node can consume corrupted metadata. */
        if (buf->samples != owned.samples || buf->capacity_samples != owned.capacity_samples ||
            buf->capacity_frames != owned.capacity_frames || buf->frame_count != p->block_frames ||
            !eaf_format_valid(&buf->format) ||
            buf->format.sample_rate != p->output_format.sample_rate ||
            buf->format.num_channels < channels ||
            buf->format.num_channels > p->output_format.num_channels)
            rc = EAF_INVALID;
    }
    if (!rc &&
        (!eaf_format_equal(&buf->format, &p->output_format) || buf->frame_count != p->block_frames))
        rc = EAF_INVALID;
    if (rc) {
        *buf = owned; /* Recover only through the sink-owned storage. */
        buf->format = p->output_format;
        buf->frame_count = p->block_frames;
        memset(buf->samples, 0,
               (size_t)p->block_frames * p->output_format.num_channels * sizeof(int32_t));
        buf->flags = EAF_FRAME_SILENCE;
    }
    if (!rc && eos)
        buf->flags |= EAF_FRAME_EOS;
    int commit = sink->ops->commit_buf(sink, buf);
    if (!rc && !commit && eos) {
        p->completed = true;
        return EAF_EOF;
    }
    return rc ? rc : commit;
}
int eaf_pipeline_stop(eaf_pipeline_t *p) {
    if (!p)
        return EAF_INVALID;
    if (p->state != EAF_RUNNING)
        return EAF_STATE;
    int rc = p->config.sink->ops->stop(p->config.sink);
    if (rc)
        return rc;
    p->state = EAF_STOPPED;
    for (size_t i = 0; i < p->initialized_nodes; ++i) {
        int reset = p->config.nodes[i]->ops->reset(p->config.nodes[i]);
        if (!rc)
            rc = reset;
    }
    return rc;
}
int eaf_pipeline_deinit(eaf_pipeline_t *p) {
    if (!p)
        return EAF_INVALID;
    if (p->state == EAF_RUNNING)
        return EAF_STATE;
    release(p);
    *p = (eaf_pipeline_t){0};
    return EAF_OK;
}
