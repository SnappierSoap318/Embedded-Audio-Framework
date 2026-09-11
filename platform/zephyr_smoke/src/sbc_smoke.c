#include <eaf/eaf_sbc_oi.h>
#include <string.h>
static const uint8_t encoded[] = {
#include "../../../tests/fixtures/tone_sbc.inc"
};
static eaf_sbc_oi_t codec;
static eaf_bt_ingress_t queue;
static eaf_bt_decoder_t worker;
static eaf_reservoir_t reservoir;
static int32_t storage[1024];
int eaf_sbc_smoke(void) {
    eaf_sbc_decoder_t decoder;
    int rc = eaf_sbc_oi_init(&codec, &decoder);
    eaf_format_t fmt = {48000, 2, 3};
    if (!rc)
        rc = eaf_reservoir_init(&reservoir, storage, 512, fmt, 256);
    eaf_bt_ingress_init(&queue);
    if (!rc)
        rc = eaf_bt_decoder_init(&worker, &queue, &reservoir, &decoder);
    uint8_t payload[sizeof(encoded) + 1u];
    payload[0] = 4;
    memcpy(payload + 1, encoded, sizeof(encoded));
    if (!rc)
        rc = eaf_bt_sbc_receive(&queue, 0, 0, payload, sizeof(payload));
    for (unsigned i = 0; !rc && i < 8; ++i)
        rc = eaf_bt_decoder_step(&worker);
    if (!rc && eaf_reservoir_level(&reservoir) != 512)
        rc = EAF_IO;
    return rc;
}
