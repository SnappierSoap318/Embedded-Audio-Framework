#include <eaf/eaf_types.h>
bool eaf_format_valid(const eaf_format_t *fmt) {
    if (!fmt || !fmt->sample_rate || !fmt->num_channels || fmt->num_channels > EAF_MAX_CHANNELS ||
        (fmt->channel_mask & ~15u))
        return false;
    uint32_t mask = fmt->channel_mask;
    unsigned count = 0;
    while (mask) {
        count += mask & 1u;
        mask >>= 1;
    }
    return count == fmt->num_channels;
}
bool eaf_format_equal(const eaf_format_t *a, const eaf_format_t *b) {
    return a->sample_rate == b->sample_rate && a->num_channels == b->num_channels &&
           a->channel_mask == b->channel_mask;
}
