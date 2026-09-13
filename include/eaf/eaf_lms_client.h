#pragma once
#include <eaf/eaf_lms.h>
#include <eaf/eaf_net.h>
#define EAF_LMS_PCM_FRAMES 512u
/* Raw source PCM retained between the socket and the output callback. It lets
   the transport keep draining the socket while the output reservoir is full,
   so the server's TCP window stays open instead of stalling on backpressure.
   Boards select the size; the default keeps the Linux harness bounded. */
#ifndef EAF_LMS_INGRESS_BYTES
#define EAF_LMS_INGRESS_BYTES 4096u
#endif
typedef struct {
    uint32_t stream_bytes, output_ms;
    uint8_t sample_bytes;
} eaf_lms_buffer_request_t;
typedef struct {
    uint32_t capacity_frames, ready_frames;
    bool clamped;
} eaf_lms_buffer_limits_t;
/* Convert both threshold units to source frames; clamp to real output capacity.
   preferred_frames is the platform's minimum startup/rebuffer reserve. */
int eaf_lms_buffer_limits(const eaf_format_t *format, const eaf_lms_buffer_request_t *request,
                          uint32_t capacity_frames, uint32_t preferred_frames,
                          eaf_lms_buffer_limits_t *limits);
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
    /* Optional stereo master gain, Q1.31 [0, INT32_MAX], exact unity at INT32_MAX.
       Transport thread must hand off to the audio owner, not mutate live DSP. */
    int (*volume)(void *, int32_t left, int32_t right);
    /* Optional pair, replaces start for buffer-aware platforms. start_buffered
       prepares a writable PCM reservoir with its audio consumer HELD. Return
       actual finite capacity/readiness. PCM/EOF may arrive before release, but
       never before cont. release starts consumption; both callbacks are owned
       by the transport thread. A failed start must unwind its own resources. */
    int (*start_buffered)(void *, const eaf_format_t *, const eaf_lms_buffer_request_t *,
                          eaf_lms_buffer_limits_t *);
    int (*release)(void *);
} eaf_lms_callbacks_t;
typedef struct {
    uint32_t elapsed_ms, buffer_bytes, queued_bytes;
    /* True only after the output owner observes actual track playback. */
    bool started;
} eaf_lms_playback_t;
typedef enum {
    EAF_LMS_STAGE_NONE,
    EAF_LMS_STAGE_CONNECT,
    EAF_LMS_STAGE_CONTROL_SEND,
    EAF_LMS_STAGE_CONTROL_RECV,
    EAF_LMS_STAGE_COMMAND,
    EAF_LMS_STAGE_HTTP_CONNECT,
    EAF_LMS_STAGE_OUTPUT_START,
    EAF_LMS_STAGE_HTTP_SEND,
    EAF_LMS_STAGE_HTTP_RECV,
    EAF_LMS_STAGE_HEADERS,
    EAF_LMS_STAGE_DECODE,
    EAF_LMS_STAGE_PCM,
    EAF_LMS_STAGE_STATUS
} eaf_lms_stage_t;
typedef struct {
    /* Session totals survive track changes and close; reset on connect attempt. */
    uint64_t http_bytes, pcm_frames;
    uint32_t recv_again, backpressure, budget_yields;
    int first_error;
    eaf_lms_stage_t error_stage;
    uint8_t error_opcode[4];
} eaf_lms_diagnostics_t;
typedef struct {
    uint32_t steps;
    bool progressed, budget_exhausted, backpressured;
} eaf_lms_pump_result_t;
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
    uint8_t ingress[EAF_LMS_INGRESS_BYTES];
    size_t ingress_used;
    eaf_format_t format;
    unsigned width;
    bool big_endian, headers_done, streaming, body_done, has_length;
    bool wait_cont, wait_start, ready_sent, started_sent;
    eaf_lms_playback_t playback;
    uint64_t remaining, bytes_received;
    eaf_lms_diagnostics_t diagnostics;
    eaf_lms_stage_t stage;
    uint8_t opcode[4];
    bool step_progress, step_backpressure;
    bool buffered_output, output_released, output_paused, input_eof, eof_sent;
    uint32_t prefill_frames;
    eaf_lms_buffer_limits_t buffer_limits;
} eaf_lms_client_t;
int eaf_lms_client_init(eaf_lms_client_t *c, const eaf_lms_callbacks_t *callbacks);
int eaf_lms_client_connect(eaf_lms_client_t *c, uint32_t server, uint16_t port,
                           const uint8_t mac[6]);
/* One bounded nonblocking I/O iteration except stream connect/start callbacks.
   Raw PCM only; caller repeats in transport worker and explicitly reconnects
   after errors. The audio task remains independent while backpressured. */
int eaf_lms_client_step(eaf_lms_client_t *c);
/* Progress-driven batch: at most 64 steps, with a checked time budget (1..10000 us).
   Services control before HTTP each step; stops on no progress. max_us bounds
   admission of another step, not the synchronous connect/start/pause callbacks.
   On budget exhaustion yield briefly; on idle/backpressure wait before retrying. */
int eaf_lms_client_pump(eaf_lms_client_t *c, uint32_t max_steps, uint32_t max_us,
                        eaf_lms_pump_result_t *result);
/* Transport-thread only: pass a coherently transferred output-owner snapshot.
   Sends STMs once per stream on started, then STMt on subsequent reports.
   Network receipt/PCM acceptance alone must not be reported as playback. */
int eaf_lms_client_report_playback(eaf_lms_client_t *c, const eaf_lms_playback_t *playback);
void eaf_lms_client_close(eaf_lms_client_t *c);
