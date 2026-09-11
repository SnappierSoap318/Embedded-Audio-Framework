#pragma once
#include <eaf/eaf_types.h>
/* Reader owns I/O; parser/decoder code has no OS dependency. read_at is exact:
   return EAF_IO on a short read. Open and close readers while stopped. */
typedef struct {
    int (*read_at)(void *ctx, uint64_t offset, void *dst, size_t bytes);
    void *ctx;
    uint64_t size;
} eaf_reader_t;
typedef struct eaf_source eaf_source_t;
struct eaf_source_ops {
    /* A successful read may return fewer frames; zero frames indicates EOF.
       No post-init allocations. Error output must not be published. */
    int (*read)(eaf_source_t *, int32_t *samples, uint32_t capacity_frames, uint32_t *frames);
    int (*seek)(eaf_source_t *, uint64_t frame);
};
struct eaf_source {
    const struct eaf_source_ops *ops;
    void *ctx;
    eaf_format_t format;
    uint64_t total_frames;
};
