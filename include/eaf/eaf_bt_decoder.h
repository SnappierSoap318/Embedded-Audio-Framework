#pragma once
#include <eaf/eaf_bt.h>
#include <eaf/eaf_reservoir.h>
typedef struct {
    int (*reset)(void *ctx);
    int (*frame)(void *ctx, const uint8_t *data, size_t length, size_t *consumed, int16_t pcm[256],
                 uint32_t *frames, eaf_format_t *format);
    void *ctx;
} eaf_sbc_decoder_t;
typedef struct {
    eaf_bt_ingress_t *ingress;
    eaf_reservoir_t *reservoir;
    eaf_sbc_decoder_t decoder;
    eaf_bt_packet_t packet;
    size_t offset;
    unsigned frames_left;
    int16_t pcm16[256];
    int32_t pcm31[256];
    uint32_t pending, sent, rejected;
} eaf_bt_decoder_t;
/* Stereo output, including mono SBC upmix. One worker owns this object and
   reservoir's producer endpoint. Configure/reset only while callbacks are parked. */
int eaf_bt_decoder_init(eaf_bt_decoder_t *, eaf_bt_ingress_t *, eaf_reservoir_t *,
                        const eaf_sbc_decoder_t *);
/* At most one SBC frame per step. Retains partial PCM writes across backpressure.
   No radio calls, no heap, no blocking. Errors drop remaining packet and reset
   history; owner decides whether to continue or terminate. */
int eaf_bt_decoder_step(eaf_bt_decoder_t *);
