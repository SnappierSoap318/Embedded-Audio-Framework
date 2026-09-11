#include <eaf/eaf_player.h>
static void wake_producer(void *ctx) {
    hal_sem_give(ctx);
}
static void produce(void *ctx) {
    eaf_player_t *p = ctx;
    eaf_reservoir_t *r = p->pipeline->config.reservoir;
    while (!hal_atomic_get(&p->gate) && !hal_atomic_get(&p->quit))
        (void)hal_sem_take(&p->wake, 20);
    bool ended = false;
    while (!hal_atomic_get(&p->quit)) {
        if (ended) {
            (void)hal_sem_take(&p->wake, 20);
            continue;
        }
        if (eaf_reservoir_backpressure(r)) {
            while (eaf_reservoir_level(r) >= r->backpressure_low && !hal_atomic_get(&p->quit))
                (void)hal_sem_take(&p->wake, 20);
        }
        if (hal_atomic_get(&p->quit))
            break;
        uint32_t frames = 0;
        int rc =
            p->source->ops->read(p->source, p->decode_buffer, EAF_PLAYER_DECODE_FRAMES, &frames);
        if (rc || frames > EAF_PLAYER_DECODE_FRAMES) {
            p->source_error = rc ? rc : EAF_INVALID;
            hal_atomic_set(&p->failed, 1);
            ended = true;
            continue;
        }
        if (!frames) {
            eaf_reservoir_finish(r);
            ended = true;
            continue;
        }
        uint32_t offset = 0;
        while (offset < frames && !hal_atomic_get(&p->quit)) {
            uint32_t n = eaf_reservoir_write(
                r, p->decode_buffer + (size_t)offset * r->format.num_channels, frames - offset);
            offset += n;
            if (!n)
                (void)hal_sem_take(&p->wake, 20);
        }
    }
}
int eaf_player_init(eaf_player_t *p, eaf_pipeline_t *pipeline, eaf_source_t *source,
                    eaf_volume_ctx_t *volume) {
    if (!p || p->initialized || !pipeline || !source || !source->ops || !source->ops->read ||
        !source->ops->seek || !eaf_format_valid(&source->format))
        return EAF_INVALID;
    if (pipeline->state != EAF_CONFIGURED && pipeline->state != EAF_STOPPED)
        return EAF_STATE;
    eaf_reservoir_t *r = pipeline->config.reservoir;
    if (!eaf_format_equal(&r->format, &source->format) || r->wake_producer ||
        !r->backpressure_low || r->backpressure_low >= r->backpressure_high ||
        r->backpressure_high > r->capacity || r->high_watermark > r->backpressure_high ||
        pipeline->block_frames > r->backpressure_high)
        return EAF_INVALID;
    if (volume) {
        bool found = false;
        for (size_t i = 0; i < pipeline->config.node_count; ++i)
            if (pipeline->config.nodes[i]->ctx == volume &&
                pipeline->config.nodes[i]->ops == &eaf_volume_ops)
                found = true;
        if (!found)
            return EAF_INVALID;
    }
    *p = (eaf_player_t){.pipeline = pipeline, .source = source, .volume = volume};
    atomic_init(&p->gate.value, 0u);
    atomic_init(&p->quit.value, 0u);
    atomic_init(&p->failed.value, 0u);
    eaf_control_init(&p->commands);
    int rc = hal_sem_init(&p->wake);
    if (rc)
        return rc;
    r->wake_producer = wake_producer;
    r->wake_ctx = &p->wake;
    p->initialized = true;
    return EAF_OK;
}
int eaf_player_start(eaf_player_t *p) {
    if (!p || !p->initialized)
        return EAF_INVALID;
    if (p->decoder.impl ||
        (p->pipeline->state != EAF_CONFIGURED && p->pipeline->state != EAF_STOPPED))
        return EAF_STATE;
    hal_atomic_set(&p->gate, 0);
    hal_atomic_set(&p->quit, 0);
    hal_atomic_set(&p->failed, 0);
    p->source_error = 0;
    int rc = hal_thread_create(&p->decoder, produce, p);
    if (rc)
        return rc;
    rc = eaf_pipeline_start(p->pipeline);
    if (rc) {
        hal_atomic_set(&p->quit, 1);
        hal_sem_give(&p->wake);
        (void)hal_thread_join(&p->decoder);
        return rc;
    }
    hal_atomic_set(&p->gate, 1);
    hal_sem_give(&p->wake);
    return EAF_OK;
}
int eaf_player_stop(eaf_player_t *p) {
    if (!p || !p->initialized)
        return EAF_INVALID;
    int rc = EAF_OK;
    if (p->pipeline->state == EAF_RUNNING || p->pipeline->state == EAF_RECOVERY)
        rc = eaf_pipeline_stop(p->pipeline);
    /* Always request producer shutdown, including sink errors. Retain the thread
       handle on sink-stop failure so callers can retry without freeing live RT resources. */
    hal_atomic_set(&p->quit, 1);
    hal_sem_give(&p->wake);
    if (rc)
        return rc;
    if (p->decoder.impl)
        rc = hal_thread_join(&p->decoder);
    return rc;
}
int eaf_player_seek(eaf_player_t *p, uint64_t frame) {
    if (!p || !p->initialized || frame > p->source->total_frames)
        return EAF_INVALID;
    bool running = p->pipeline->state == EAF_RUNNING;
    int rc = eaf_player_stop(p);
    if (rc)
        return rc;
    rc = p->source->ops->seek(p->source, frame);
    if (rc)
        return rc;
    eaf_reservoir_reset(p->pipeline->config.reservoir);
    return running ? eaf_player_start(p) : EAF_OK;
}
bool eaf_player_command(eaf_player_t *p, const eaf_command_t *command) {
    if (!p || !p->initialized || !command)
        return false;
    switch (command->type) {
    case EAF_COMMAND_STOP:
        break;
    case EAF_COMMAND_SEEK:
        if (command->frame > p->source->total_frames)
            return false;
        break;
    case EAF_COMMAND_GAIN:
        if (!p->volume || command->gain < 0)
            return false;
        break;
    default:
        return false;
    }
    return eaf_control_push(&p->commands, command);
}
int eaf_player_step(eaf_player_t *p) {
    if (!p || !p->initialized)
        return EAF_INVALID;
    eaf_command_t command;
    if (eaf_control_pop(&p->commands, &command)) {
        int rc = EAF_OK;
        switch (command.type) {
        case EAF_COMMAND_STOP:
            rc = eaf_player_stop(p);
            return rc ? rc : EAF_EOF;
        case EAF_COMMAND_SEEK:
            rc = eaf_player_seek(p, command.frame);
            break;
        case EAF_COMMAND_GAIN:
            for (size_t i = 0; i < EAF_MAX_CHANNELS; ++i)
                p->volume->gain[i] = command.gain;
            break;
        default:
            return EAF_INVALID;
        }
        if (rc)
            return rc;
    }
    if (p->pipeline->state != EAF_RUNNING)
        return EAF_STATE;
    int rc;
    if (hal_atomic_get(&p->failed))
        rc = p->source_error;
    else
        rc = eaf_pipeline_process(p->pipeline);
    if (rc) {
        int stopped = eaf_player_stop(p);
        if (stopped)
            return stopped;
    }
    return rc;
}
int eaf_player_deinit(eaf_player_t *p) {
    if (!p || !p->initialized)
        return EAF_INVALID;
    int rc = eaf_player_stop(p);
    if (rc)
        return rc;
    p->pipeline->config.reservoir->wake_producer = NULL;
    p->pipeline->config.reservoir->wake_ctx = NULL;
    hal_sem_deinit(&p->wake);
    p->initialized = false;
    return EAF_OK;
}
