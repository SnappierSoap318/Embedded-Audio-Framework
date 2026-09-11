# Repository audit — 2026-09-11

Baseline: `7114eb3`, following the user-confirmed stereo-volume fix. Reviewed the
portable core, WAV/player, LMS/SBC transports, Linux/Zephyr HAL and sinks,
platform examples, build configuration, test coverage and v0.4 design. This is a
source/contract audit with targeted tests, not proof that the repository is bug-free.
External codec internals and physical hardware need separate qualification.

## Findings

| ID | Severity | Finding and evidence | Disposition |
| --- | --- | --- | --- |
| A01 | High | `core/eaf_graph.c` trusted mutable node buffer pointers/capacities after processing; failure cleanup could use the altered pointer. | Fixed: preserve sink storage metadata, check each node boundary, restore before silence/commit. Fault-node tests failed before the fix. Cannot protect against a node's own arbitrary out-of-bounds write. |
| A02 | Medium | `core/eaf_dsp.c` multiplied finite large Hz by 2π before dividing by rate; overflow led to NaN-to-int UB. | Fixed: normalize first, reject nonfinite coefficients. UBSan reproduced with DBL_MAX; normalized-reference regression passes. |
| A03 | High | `platform/native_linux/play_lms.c:start` creates the audio thread after pipeline START; the Linux HAL allocates for it. | Open, T02. Conflicts with the literal post-START no-heap invariant. Create/gate worker before START, then release it. |
| A04 | High | Generic sink deinit is void; I2S retains state when DROP fails, but graph release cannot learn that. Native LMS cleanup also ignores stop/join/deinit errors. | Open, T03. Define recoverable/fatal cleanup and retain ownership on errors. Add injected driver-failure tests before changing API. |
| A05 | High | `strm` thresholds are ignored, STMl means one local chunk, jiffies/stream-buffer STAT fields remain zero, and decode EOF is not output completion. | Open, T10/T11. The current client cannot claim complete LMS sequencing, gapless playback or sync. |
| A06 | High | A2DP suspend/release gates new packets but leaves queued old packets; application source switching/reset coordination does not exist. | Open, T08. An endpoint binding is not a finished BT player. Keep single producer/consumer ownership during purge. |
| A07 | Medium | Native player feature `#ifdef EAF_HAVE_ALSA` branches and POSIX CLI includes live in platform code. | Open, T01. Conflicts with strict sections 1/12, though core/apps remain portable. Move backend selection into CMake-selected adapters; keep CLI platform exception explicit until then. |
| A08 | Medium | DSP uses Q1.31 histories, not extended 32x64 recursive state; dither, EQ controls, ASRC and precision/noise qualification are absent. | Open, T17–T20. Existing impulse/waveform tests do not establish low-frequency quality. |
| A09 | Medium | Linux semaphore deadlines use CLOCK_REALTIME; wall-clock changes can perturb waits. Zephyr thread priority is one global setting with no affinity API. | Open, T04. Clarify HAL timing/scheduling contracts and add OS tests. |
| A10 | Medium | ALSA write/drain waits are bounded but far longer than an audio block; plugins may allocate and buffered delay is not speaker-time proof. I2S STOP drops the final hardware tail. | Open, T03/T09/T21. No hard-real-time or embedded EOF-drain claim. |
| A11 | High | Architecture calls for 200–500 ms stereo reservoirs, but WROOM internal SRAM must also fit Wi-Fi/BT, stacks, protocol buffers and DMA. | Open, T05/T06. 48 kHz stereo Q31 costs 76,800 bytes/200 ms or 192,000 bytes/500 ms for samples alone. Measure a link map before choosing capacity. |
| A12 | Medium | Mock I2S/A2DP tests do not validate ESP32 clocks, PSRAM DMA reachability, controller/host coexistence or pairing. | Open, T05–T08/T22. Board support is the next integration milestone. |

## Design reconciliation

The design is an objective, not a description of current completeness. Important
inconsistencies inside `arch.md` itself include volatile atomics (unsafe example),
signed sample shifts (undefined for negatives), non-power-of-two reservoir size
in the board example, old API signatures, and PREBUFFERING described as returning
zero frames while the current contract always fills the requested block with
silence. `PLAN.md` records the chosen semantics, including deliberate underrun
tail discard and source EOF draining. These decisions should become a v0.5 spec
rather than silently changing v0.4 historical examples.

Static slabs are bounded ownership transfer, but conflict with a literal ban on
all pool operations. Clarify that circulating fixed DMA blocks is allowed while
growing pools/heap calls are not. Separate the audio processing guarantee from
setup/reconfiguration and third-party Linux plugin behavior. The repository's
malloc-wrap test verifies direct linked graph-path calls only.

The graph now restores EOS after DSP so a node cannot accidentally suppress sink
drain, and rejects buffer metadata corruption before calling subsequent nodes.
This does not replace per-node input validation or numerical testing.

## Reference-guided test strategy

Local reference: `squeezelite-esp32`, commit
`1d542bd53cf397661f11c0671553422acdb663ab`. Reviewed
`components/squeezelite/{slimproto.h,slimproto.c,stream.c,pcm.c}` and
`components/driver_bt/bt_app_sink.c`. Use wire/state facts, not wholesale GPL source
imports into an otherwise separately implemented framework.

- Wire fixtures must encode field widths independently of the implementation.
  A literal audg vector now complements packed-field fixtures. Sequence fields
  deliberately differ from gains; the previous fixture mirrored a parser bug.
- Expand fixtures to HELO/STAT/strm/cont/RESP, with version/provenance notes and
  explicit expected bytes, truncation at every boundary and fragmented delivery.
- Reproduce reference lifecycle ordering: first output vs decode completion,
  pause/start-at, buffering, track replacement, reconnect and server timestamps.
  Unsupported features need deterministic rejection tests until implemented.
- Do not equate ESP-IDF's decoded-PCM A2DP callback with Zephyr's SBC-media callback.
  Test SBC framing/discontinuity and output ownership at their actual boundaries.
- Run fault injection for partial socket/ALSA writes, timeout, xrun, driver DROP
  failure, queued control bursts, and source switches with a full reservoir.
- `test_architecture.py` now rejects direct OS/SDK includes and feature conditionals
  in portable core/apps C. It is a guardrail, not a full layering proof; current
  platform exceptions remain visible above.

For future changes: state the contract, add an independent regression that fails
before the fix, implement the smallest correction, run affected unit/integration
checks, then ASan/UBSan and the relevant target build. Add TSan for concurrency
changes. Save hardware measurements separately from simulator results and commit
validated increments. Test gates and completion criteria are in `TASKS.md`.
