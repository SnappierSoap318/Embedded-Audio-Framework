#pragma once
#include <eaf/eaf_reservoir.h>
typedef enum {
    EAF_NODE_STAGE_PRE_PROCESS,
    EAF_NODE_STAGE_DSP,
    EAF_NODE_STAGE_POST_PROCESS
} eaf_node_stage_t;
typedef struct eaf_node eaf_node_t;
struct eaf_node_ops {
    int (*init)(eaf_node_t *, const eaf_format_t *, eaf_format_t *);
    int (*process)(eaf_node_t *, eaf_buffer_t *);
    int (*reset)(eaf_node_t *);
    void (*deinit)(eaf_node_t *);
};
struct eaf_node {
    const char *name;
    eaf_node_stage_t stage;
    const struct eaf_node_ops *ops;
    void *ctx;
};
typedef enum {
    EAF_UNINITIALIZED,
    EAF_INITIALIZED,
    EAF_CONFIGURED,
    EAF_RUNNING,
    EAF_STOPPED
} eaf_pipeline_state_t;
typedef struct {
    eaf_reservoir_t *reservoir;
    eaf_node_t *const *nodes;
    size_t node_count;
    eaf_sink_t *sink;
} eaf_pipeline_config_t;
typedef struct {
    eaf_pipeline_config_t config;
    eaf_pipeline_state_t state;
    eaf_format_t output_format;
    uint32_t block_frames;
    size_t initialized_nodes;
    bool sink_initialized;
    bool completed;
} eaf_pipeline_t;
/* Pipeline must be zero initialized. Caller serializes lifecycle and process. */
int eaf_pipeline_init(eaf_pipeline_t *p, const eaf_pipeline_config_t *config);
int eaf_pipeline_configure(eaf_pipeline_t *p, uint32_t block_frames);
int eaf_pipeline_start(eaf_pipeline_t *p);
/* Returns EAF_EOF once the final padded source block is committed, and on later calls. */
int eaf_pipeline_process(eaf_pipeline_t *p);
int eaf_pipeline_stop(eaf_pipeline_t *p);
int eaf_pipeline_deinit(eaf_pipeline_t *p);
