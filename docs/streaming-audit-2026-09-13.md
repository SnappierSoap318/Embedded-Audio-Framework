# Streaming audit against squeezelite-esp32 — 2026-09-13

Baseline: EAF `e1b7e2712c8316da9d749dacb0e3ee501cbe12f2` and the local
`sle118/squeezelite-esp32` checkout at
`1d542bd53cf397661f11c0671553422acdb663ab`.

The reference offers useful patterns for immediate standalone playback improvements.
A full SlimProto implementation is not a prerequisite for fixing receive scheduling,
startup buffering and stall diagnosis. Conversely, increasing TCP buffers alone
cannot complete playback lifecycle or synchronization.

This is a source/contract comparison, arithmetic analysis and review of existing
regressions. It does not establish the cause of the reported 30-second dropout,
a memory leak, or physical amplifier correctness. No reference code was copied,
no firmware was changed, and no server playback was changed during this audit.
Links into EAF refer to the baseline above; reference links pin the exact revision.

## Findings and transferable patterns

### S01 — High: receive scheduling caps supported PCM throughput

EAF [`pump_http`](../apps/lms/lms_client.c#L268) performs at most one HTTP recv per
step, with a [1024-byte RX array](../include/eaf/eaf_lms_client.h#L31). The board
[unconditionally sleeps 2 ms](../platform/esp32_lms/src/main.c#L132) after each
step, even when data and reservoir space are immediately available. Its optimistic
ceiling is `1024 / 0.002 = 512000 bytes/s`, before processing, scheduling and
backpressure. 96 kHz stereo requires 576000 B/s at 24 bits and 768000 B/s at 32 bits.
Both formats pass the client's format validation, and HELO advertises 96 kHz.
This is a demonstrated design limit, not a measured board benchmark. 44.1 kHz
stereo at 16 bits needs only 176400 B/s, so this limit alone does not explain that
observed dropout. Mono expansion can require additional iterations as well.

The reference [stream worker](https://github.com/sle118/squeezelite-esp32/blob/1d542bd53cf397661f11c0671553422acdb663ab/components/squeezelite/stream.c#L310)
waits for readiness and receives into available contiguous stream-ring space.
It does have sleeps when idle/full; its useful pattern is progress-driven intake,
not copying its sleep durations or removing all waits.

**Adapt:** expose progress/would-block/backpressure from an EAF pump. Process a
bounded byte/time budget while progress is possible, servicing control traffic
between batches. Wait on socket readiness or producer-space notification when
blocked. Start with the existing sole transport owner; a new task is not required
for the first improvement. Do not replace the sleep with an unbounded spin loop.
Until throughput is qualified, constrain advertised/accepted formats consistently.

**Regression:** paced 44.1/48/96 kHz streams, all accepted widths, partial recv/send,
full reservoir and no-data periods. Verify sample counts, bounded control-command
latency and zero busy spinning. Then run on ESP32 for multiple track lengths.

### S02 — High: startup reserve is small and LMS thresholds are omitted

[`eaf_lms_stream_t`](../include/eaf/eaf_lms.h#L4) and
[`eaf_lms_parse_stream`](../apps/lms/lms_protocol.c#L46) omit stream threshold and
output threshold fields. The board always uses
[4096 reservoir frames and a 1024-frame high watermark](../platform/esp32_lms/src/output.c#L125).
`STMl` is sent after one private decoded chunk (or body EOF), rather than the
reference's stream-buffer/decode-readiness transition.

| Sample rate | Entire 4096-frame reservoir | 1024-frame start reserve | Four 128-frame DMA blocks, nominal maximum |
| --- | ---: | ---: | ---: |
| 44.1 kHz | 92.88 ms | 23.22 ms | 11.61 ms |
| 48 kHz | 85.33 ms | 21.33 ms | 10.67 ms |
| 96 kHz | 42.67 ms | 10.67 ms | 5.33 ms |

These durations are arithmetic capacities, not proof all DMA slots are queued.
A higher watermark changes when playback begins; it does not increase capacity
or cure persistently insufficient average throughput.

The reference [parses both thresholds](https://github.com/sle118/squeezelite-esp32/blob/1d542bd53cf397661f11c0671553422acdb663ab/components/squeezelite/slimproto.c#L372):
stream threshold uses KiB; output threshold uses tenths of a second in
[output buffering](https://github.com/sle118/squeezelite-esp32/blob/1d542bd53cf397661f11c0671553422acdb663ab/components/squeezelite/output.c#L62).
Its controller treats stream buffering, decoder readiness and audible start as
separate conditions.

**Adapt:** parse both wire fields with independent fixtures. Define a feasible
WROOM buffering policy in frames/time, expose actual capacity, and distinguish
ready-to-decode from ready-to-play. Define explicit behavior when the requested
threshold exceeds capacity; silently waiting for an impossible threshold would
introduce a deadlock. EOF-short tracks must still play and drain.

**Regression:** threshold boundaries, all four autostart modes, delayed `cont`,
short EOF below watermark, pause while prebuffering, and impossible thresholds.
Inject 20/50/100/200 ms gaps with enough average delivery bandwidth; measure the
actual reserve required instead of declaring a particular watermark sufficient.

### S03 — High: output starvation and final completion are not protocol events

EAF [HTTP EOF handling](../apps/lms/lms_client.c#L296) queues reservoir EOF and sends
`STMd`. That event can correctly represent decoder completion; moving it blindly
to hardware drain would also be wrong. The missing part is separate output state:
`done` stays [inside the board output module](../platform/esp32_lms/src/output.c#L81),
while the transport can keep reporting `STMt` indefinitely for a finished stream.
There are no `STMo`, `STMu` or `DSCO` emissions. A new `strm s` calls
`stop_stream`, whose callback can DROP queued old output, so early next-track
arrival can truncate the tail; gapless handoff is not implemented.

The reference [controller](https://github.com/sle118/squeezelite-esp32/blob/1d542bd53cf397661f11c0671553422acdb663ab/components/squeezelite/slimproto.c#L710)
separates `STMd` (decode complete), `STMo` (output empty while HTTP continues),
`STMu` (output empty after stream/decode completion) and `DSCO` (stream disconnect
reason), alongside `STMs` when the track begins output.

**Adapt:** transfer a coherent output state snapshot/event to the transport owner:
prebuffering, playing, starved, paused, draining, drained and failed. Send each
transition once with an explicit re-arm policy. Keep decode EOF and output drain
separate. Implement sequential track-tail preservation before pursuing gapless
track markers/crossfade.

**Regression:** HTTP EOF while multiple output blocks remain; EOF with a short
partial final block; live stream starvation and recovery; next-track request
before drain; explicit stop during drain. Assert exact event order/count and no
lost/duplicated PCM. Current peer tests stop at `STMd`, so they do not cover this.

### S04 — High for sync, medium for standalone: STAT is only partially populated

[`status`](../apps/lms/lms_client.c#L23) zero-initializes its payload and leaves
stream-buffer size/fullness and jiffies zero. Output time derives from reservoir
frames consumed after commit, without subtracting hardware queue delay. This is
not a presentation clock. Incomplete fields may affect server decisions, but the
audit has not shown they cause the particular 30-second failure.

Reference [STAT generation](https://github.com/sle118/squeezelite-esp32/blob/1d542bd53cf397661f11c0671553422acdb663ab/components/squeezelite/slimproto.c#L164)
reports stream/output occupancy, a monotonic timestamp and output time adjusted
for queued device frames. Its ESP-IDF I2S backend uses driver-specific timing
assumptions; those are not portable guarantees for Zephyr.

**Adapt:** populate real monotonic jiffies now. Define what EAF's stream buffer is
before reporting size/fullness; do not invent kernel TCP occupancy or double-count
Q31 storage. Add a HAL presentation/queued-frame capability where supported and
label estimates until physically verified. Use independent complete STAT vectors.

### S05 — Medium: network intake and PCM conversion lack an independent reserve

EAF receives and converts within one transport step, then stops reading while its
private PCM chunk cannot be published. This is bounded backpressure, not evidence
of a leak, but it gives network jitter little buffering beyond the Q31 reservoir.
The reference has [stream and output rings](https://github.com/sle118/squeezelite-esp32/blob/1d542bd53cf397661f11c0671553422acdb663ab/components/squeezelite/squeezelite.h#L254)
and a [decoder worker](https://github.com/sle118/squeezelite-esp32/blob/1d542bd53cf397661f11c0671553422acdb663ab/components/squeezelite/decode.c#L60)
that runs according to input availability/output space.

**Adapt after S01 measurements:** evaluate a small static raw-byte ingress ring.
For stereo 16-bit PCM, raw samples use half the memory of stereo Q31, so compact
staging can improve the reserve per byte. At 32 bits it offers no sample-storage
saving. Budget the ring together with TCP buffers, stacks and DMA; adding a ring
is not free, and faster pumping may be sufficient without it. Keep one producer
and consumer per ring and no live allocation in the audio path.

Do not transplant the reference's embedded defaults: 480 KiB stream storage and
1450 KiB output storage exceed this WROOM's available internal memory. Its
[I2S constants](https://github.com/sle118/squeezelite-esp32/blob/1d542bd53cf397661f11c0671553422acdb663ab/components/squeezelite/output_i2s.c#L69)
also use 512 frames × 12 blocks, unlike EAF's 128 × 4. The adjacent old comment
mentions six buffers; use the actual constants, not the comment. Measure Zephyr
DMA queue margin before resizing; ESP-IDF task priorities and DMA assumptions
must not be copied numerically.

### S06 — Medium: no established-connection progress watchdog

EAF applies a [500 ms connect timeout](../apps/lms/lms_client.c#L133), but repeated
`EAF_AGAIN` on established sockets can continue indefinitely. A healthy control
connection with a stalled HTTP producer can leave playback silent without an
explicit recovery decision. The fixed five-second retry only begins after an error
is actually returned. Time-bounded socket operations are not a playback liveness
policy.

The reference [control loop](https://github.com/sle118/squeezelite-esp32/blob/1d542bd53cf397661f11c0671553422acdb663ab/components/squeezelite/slimproto.c#L650)
returns after repeated missing-server-message timeouts, and carries stream
disconnect reasons separately. It is not evidence of a universal HTTP stall cure.
Its connect timeouts are also much longer than EAF's; copy neither blindly.

**Adapt:** track last control activity, HTTP progress, output progress and retry
reason independently. A stall policy must exclude intentional pause, wait-for-cont,
wait-for-start, full-buffer backpressure and drain. Report the reason and take a
bounded recovery action without repeatedly reallocating resources.

**Regression:** silent but open HTTP peer, half-open control connection, delayed
connect, pause longer than watchdog, server restart, repeated reconnect and stable
socket/slab/thread counts. Retain heap minimum and allocation-failure counts on
hardware to distinguish resource exhaustion from packet loss or server failure.

### S07 — Medium: underrun policy trades continuity for recovery

The [reservoir](../core/eaf_reservoir.c#L102) fades from the previous sample, discards
all remaining input below one block, then re-enters prebuffering. This is an
explicit current architecture contract, not an accidental out-of-bounds bug.
Repeated starvation therefore also loses up to 127 source frames each time with
128-frame blocks, worsening continuity and complicating source-time accounting.
The reference [output loop](https://github.com/sle118/squeezelite-esp32/blob/1d542bd53cf397661f11c0671553422acdb663ab/components/squeezelite/output.c#L111)
can consume partial available frames and insert silence when no frames remain.

**Adapt carefully:** consider a music-specific policy that preserves partial PCM
and pads output, with explicit rebuffer hysteresis. Keep live-input latency policy
separate. Change architecture and regression expectations together; do not silently
alter the shared reservoir for BT. First measure whether starvation is the cause.

### S08 — Confirmed sync limitation, not a standalone fix

[Timestamped resume/pause](../apps/lms/lms_client.c#L90) returns EAF_UNSUPPORTED,
which closes the connection through the parser/client error path. The reference
[schedules start/pause/skip](https://github.com/sle118/squeezelite-esp32/blob/1d542bd53cf397661f11c0671553422acdb663ab/components/squeezelite/output.c#L69)
against output frames and monotonic time. EAF also needs S04's status clock and
hardware delay accounting before timed commands can mean synchronized playback.

Keep standalone qualification separate. Implement explicit unsupported-command
reporting, then frame/timestamp scheduling, clock correction and multi-player
validation as a later series. Merely accepting a timestamp and starting immediately
would conceal the limitation rather than implement sync.

### S09 — Diagnostics and framing gaps can hide the real failure

The logger stores recent application events but does not identify the failing
SlimProto opcode, HTTP status/content type, per-stage errno, TCP retransmissions,
minimum queue occupancy or stack/heap low-water marks. The
[generic command-failure message](../platform/esp32_lms/src/main.c#L113) mentions
sync even for unrelated errors. Five-second snapshots can miss brief queue collapse;
synchronous UART output in [board_log](../platform/esp32_lms/src/log_buffer.c#L32)
also needs timing measurement before increasing log frequency.

Reference [PCM framing](https://github.com/sle118/squeezelite-esp32/blob/1d542bd53cf397661f11c0671553422acdb663ab/components/squeezelite/pcm.c#L79)
can parse WAV/AIFF, but network header checking is conditional, not always on.
EAF converts every accepted HTTP body byte as raw PCM and rejects several HTTP
encodings. The earlier 272-byte response has not been captured with enough context
to identify its contents; do not conclude it was a WAV header or a server error.

**Adapt:** preserve the first failure cause and record bounded counters/minima
instead of high-frequency text in the audio worker. Capture sanitized HTTP status,
content type and declared length, plus actual negotiated sample format. Add a
bounded, explicit framing policy only for formats the client claims to support;
do not indiscriminately guess based on the first raw samples. Never log credentials,
auth headers or full media URLs.

The low volume has a separate known cause: [fixed 1/8 gain](../platform/esp32_lms/src/output.c#L14),
approximately −18.06 dB, before LMS master gain. It does not diagnose streaming.
Make bench attenuation configurable in a separate change with gain tests.

## Recommended implementation sequence

| Stage | Scope | Gate before proceeding | Existing tasks |
| --- | --- | --- | --- |
| R1 | S01 bounded progress-driven pump plus S09 first-failure and queue/throughput counters | Host rate/backpressure tests and a controlled 44.1/48 kHz board stream; no control starvation | T04, T07, T12 |
| R2 | S02 feasible startup/rebuffer thresholds, then S05 compact staging only if measurements justify it | Burst-gap matrix, short EOF, memory budget and no impossible-threshold wait | T06, T07, T11 |
| R3 | S03 lifecycle events, S04 truthful STAT, and S06 progress watchdog | Independent wire/event tests, track-tail preservation and server-restart tests | T10–T12 |
| R4 | S07 continuity policy, framing/format negotiation, configurable bench gain | Policy-specific sample tests and actual LMS format captures | T13, T15, T21 |
| R5 | Timed sync, presentation clock, clock correction and coexistence | Standalone playback stable first; physical multi-player measurements | T04, T08, T09, T19, T20 |

Begin with R1, not a wholesale port or a larger heap. Keep the existing static
ownership model, HAL boundaries, checked teardown and separate audio worker.
Reference polling/mutex/heap choices belong to its platform and architecture.

For the first physical run use a known local stereo PCM file, no sync, fixed
44.1/48 kHz, and known volume. Compare browser closed versus two-second polling;
collect bytes/s, queue minimum, underrun increments, transport errors and output
progress for at least ten minutes and multiple track changes. Add controlled
network gaps and server restart after that baseline. If LMS is unavailable, a
controlled SlimProto/HTTP test peer can isolate transport and I2S without assuming
that the production server or circuit is correct.

## Existing validation and missing coverage

During this audit, `board_log`, `board_output`, `reservoir`, `web_page` and
`lms_peer` passed in the native ASan/UBSan `build-review` tree (five tests).
The peer fixture exercises fragmentation, PCM widths, gates and immediate
pause/resume, but it is a short correctness test without ESP32 scheduling/network
constraints. These passes do not validate sustainable intake rate, full STAT,
output-complete events, long stalls or sync. Proposed regressions above should
fail against the relevant baseline behavior before implementation changes.
