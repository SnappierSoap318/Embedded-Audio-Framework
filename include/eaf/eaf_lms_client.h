#pragma once
#include <eaf/eaf_lms.h>
#include <eaf/eaf_net.h>
#define EAF_LMS_PCM_FRAMES 512u
typedef struct {
    /* Called on transport thread, BEFORE any PCM for the new track. Must arrange
       quiescent graph configure/start and return only when the producer may write. */
    int (*start)(void *, const eaf_format_t *);
    uint32_t (*pcm)(void *, const int32_t *, uint32_t);
    void (*eof)(void *);
    /* Must quiesce/reset the old stream; also called on disconnect/errors. */
    void (*stop)(void *);
    void *ctx;
    /* Optional synchronous handoff to output owner. Preserve queued PCM.
       Return only after pause/resume takes effect; no timed pauses supported. */
    int (*pause)(void *, bool paused);
} eaf_lms_callbacks_t;
typedef struct {
    uint32_t elapsed_ms, buffer_bytes, queued_bytes;
    /* True only after the output owner observes actual track playback. */
    bool started;
} eaf_lms_playback_t;
typedef struct {
    eaf_tcp_t control, http;
    eaf_lms_parser_t parser;
    eaf_lms_callbacks_t callbacks;
    uint32_t server;
    uint8_t tx[4096], request[EAF_LMS_MAX_PACKET], rx[1024], tail[8];
    char header[EAF_LMS_MAX_PACKET + 1u];
    size_t tx_used, tx_sent, request_used, request_sent, header_used, tail_used;
    int32_t pcm[EAF_LMS_PCM_FRAMES * 2u];
    uint32_t pcm_count, pcm_sent;
    eaf_format_t format;
    unsigned width;
    bool big_endian, headers_done, streaming, body_done, has_length;
    bool wait_cont, wait_start, ready_sent, started_sent;
    eaf_lms_playback_t playback;
    uint64_t remaining, bytes_received;
} eaf_lms_client_t;
int eaf_lms_client_init(eaf_lms_client_t *c, const eaf_lms_callbacks_t *callbacks);
int eaf_lms_client_connect(eaf_lms_client_t *c, uint32_t server, uint16_t port,
                           const uint8_t mac[6]);
/* One bounded nonblocking I/O iteration except stream connect/start callbacks.
   Raw PCM only; caller repeats in transport worker and explicitly reconnects
   after errors. The audio task remains independent while backpressured. */
int eaf_lms_client_step(eaf_lms_client_t *c);
/* Transport-thread only: pass a coherently transferred output-owner snapshot.
   Sends STMs once per stream on started, then STMt on subsequent reports.
   Network receipt/PCM acceptance alone must not be reported as playback. */
int eaf_lms_client_report_playback(eaf_lms_client_t *c, const eaf_lms_playback_t *playback);
void eaf_lms_client_close(eaf_lms_client_t *c);
