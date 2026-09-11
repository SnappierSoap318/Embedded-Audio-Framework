#pragma once
/* Optional adapter, available when building with the pinned Zephyr libsbc. */
#include <eaf/eaf_bt_decoder.h>
#include <oi_codec_sbc.h>
typedef struct {
    OI_CODEC_SBC_DECODER_CONTEXT context;
    uint32_t data[CODEC_DATA_WORDS(2, SBC_CODEC_FAST_FILTER_BUFFERS)];
} eaf_sbc_oi_t;
int eaf_sbc_oi_init(eaf_sbc_oi_t *, eaf_sbc_decoder_t *);
