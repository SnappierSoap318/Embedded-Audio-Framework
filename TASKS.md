# Completion checklist

This is the authoritative remaining-work list; `PLAN.md` records design decisions.
Audit findings and evidence: [docs/audit.md](docs/audit.md). Hardware sequence:
ESP32-WROOM + two MAX98357 modules first; ordered WROVER-IE/N16R8 + TAS5805M later.
Exact module markings, flash/PSRAM geometry, amplifier variants and pins await
confirmation. Completed Linux stereo playback is user-confirmed.

## Release gates

**Embedded stereo MVP:** a reproducible board build boots, plays LMS raw PCM and
Classic A2DP SBC through both physical channels, switches sources safely, handles
volume/mute/pause/reconnect, and passes a 24-hour stress run with recorded memory,
xrun and timing counters. Complete T01–T16, T21–T24 for the chosen board, or record
an explicit scoped exception. A working Linux player is not this release gate.

**Full v0.4 direction:** additionally complete DSP quality/controls, hardware memory
qualification, PTS/clock control and measured multi-room synchronization. AES67,
other targets and LE Audio are separate deliverables, not implied by A2DP/LMS.
There is no fixed finish date until these scopes and hardware gates are validated.

## Architecture and ownership — P0 before board integration

- [x] **T01 — Reconcile architecture v0.5.** Fold PLAN decisions into current APIs,
  fixed-pool exception, heap scope, EOF/hysteresis semantics and source ownership.
  Replace platform feature branches with CMake-selected adapters. Accept when
  examples compile and architecture tests catch forbidden dependencies.
  Completed: docs/architecture-v0.5.md, compiled finite-source example, CMake-selected
  native ALSA/null adapters and expanded portable/public-header/native source guards.
- [x] **T02 — Pre-create LMS audio worker.** Gate the worker before START; unwind
  creation/start errors without live allocations or writes after reset. Verify
  allocation interception during startup-to-first-block and repeated track changes.
  Completed: real native callbacks tested with thread-create/START failure injection,
  eight rate-changing restarts, direct heap-call guards and a clean TSan run.
- [x] **T03 — Unify sink error/drain lifecycle.** Propagate stop/deinit failures,
  retain resources safely, define bounded output drain vs drop, and avoid freeing
  thread handles until output is quiescent. Inject write/START/DROP/drain failures.
  Completed: checked sink deinit, partial-START rollback, recovery state, bounded
  ALSA/I2S queue drain and retry-safe DROP. Host faults cover partial init, START,
  write, DROP, cleanup and drain timeout; Zephyr mocks cover write/START/DROP/drain
  errors. Physical DMA completion and amplifier tail validation remain T09.
- [ ] **T04 — Complete HAL scheduling contract.** Monotonic waits, distinct decoder
  and audio priorities, optional affinity, bounded wakeups. Verify timeout behavior,
  semaphore races and contention; measure scheduling margin on ESP32.
  Software implemented: monotonic timeout result, role-aware preemptive priorities,
  optional CPU affinity, checked Linux FIFO requests, and host/Zephyr regression
  tests. Remaining acceptance: measure wake latency/DSP margin on the ESP32 under
  simultaneous Wi-Fi/BT load; keep T04 open until those results are recorded.

## First bench: WROOM + MAX98357 — P0/P1

- [ ] **T05 — Reproducible real-board build.** Confirm module/board target and
  MAX98357A/B; pin SDK/toolchain/modules, add board app, overlay and documented
  GPIO mapping. Build/flash from a clean workspace and save boot log/link map.
  Physical UART-only boot PASS: ESP32-D0WD-V3 rev 3.1, 4 MB flash, PSRAM disabled;
  4096 source frames and zero underruns. Build pins and boot/link evidence are in
  docs/bench/wroom-boot-2026-09-12.md. DAC wiring and carrier markings remain to
  be confirmed before completing the board/pin configuration.
- [ ] **T06 — Memory budget and I2S-only stereo.** Start without Wi-Fi/BT. Size
  reservoir for actual internal RAM; keep DMA slab internal. Play left-only,
  right-only, silence and low-level test tones at 44.1/48 kHz, 32-bit slots.
  Verify MAX channel-select straps and measured BCLK/LRCLK; measure block execution
  below 40% of period as the architecture requires.
- [ ] **T07 — Network LMS board app.** Add Wi-Fi provisioning/configuration, static
  workers and source/graph owner handoff. Validate TCP/HTTP PCM, gain/mute/pause,
  sample-rate changes and network stalls using the user's server.
  Software app and host regressions are implemented in `platform/esp32_lms`;
  see its README for credentials and build steps. Physical Wi-Fi/DHCP and LMS
  registration passed (see `docs/bench/wroom-lms-2026-09-12.md`); server playback,
  runtime memory and recovery checks remain pending.
  Wireless application logs are available on port 80. Qualify the enlarged RX
  pool/window with standalone playback beyond several track lengths, including
  browser polling; retain underrun counts and stalled/recovered request results.
  One 44.1 kHz/16-bit track reached real-time intake on 2026-09-13 after the
  RX-window/ingress-ring change; several track lengths, a browser-polling
  comparison, AP/server restart and automatic recovery remain.
- [ ] **T08 — Complete Bluetooth board app.** Add controller enable, SDP record,
  pairing/discoverability and bounded notification handoff. Coordinate queue purge,
  decoder reset and source arbitration across start/suspend/release/disconnect.
  Test real-phone reconnect and Wi-Fi/BT coexistence; only one PCM producer active.
- [ ] **T09 — I2S drain/recovery and clock behavior.** Implement/verify EOF drain,
  driver queue starvation recovery and clean reconfigure. Scope clocks and prove
  DMA ownership/cache correctness on the actual ESP32 driver.

## Streaming stabilization order — current priority

The [2026-09-13 reference audit](docs/streaming-audit-2026-09-13.md) compares
EAF `e1b7e27` with pinned squeezelite-esp32 source. Work in this order before
attempting synchronized playback; the underlying T tasks below remain open.

- [ ] **R1 — Intake and evidence:** bounded progress-driven receive pumping,
  first-failure attribution, throughput and queue-minimum counters. Cover accepted
  PCM rates/widths, full-buffer backpressure and bounded control latency (T04/T07/T12).
  Software pump/counters and deterministic regression are implemented. Physical
  WROOM 44.1 kHz/16-bit playback after widening the RX window and adding the
  ingress ring sustained 175-177 KB/s (real-time), advanced played time at
  +5000 ms per 5 s and held 0 underruns for ~50 s then 1 total over ~85 s,
  versus 198 across a prior track. See
  [ingress-ring evidence](docs/bench/wroom-lms-ingress-2026-09-13.md).
  Multiple track lengths, controlled gaps and a browser-polling comparison remain.
- [ ] **R2 — Buffer readiness:** parse stream/output thresholds, define feasible
  WROOM start/rebuffer policy and short-EOF behavior. Add compact ingress storage
  only if measured jitter/memory accounting justifies it (T06/T07/T11).
  Software threshold conversion, capacity clamping, held prefill and short EOF
  are implemented and regression-tested. Measured starvation justified a compact
  raw ingress ring (`EAF_LMS_INGRESS_BYTES`, 16 KiB on WROOM), now implemented and
  physically validated. Underrun-tail policy and long-play gates remain (R4).
- [ ] **R3 — Standalone lifecycle:** distinguish decode completion, output starvation
  and final drain; complete truthful STAT fields and bounded progress watchdogs.
  Test early next-track arrival, half-open connections and server restart (T10–T12).
- [ ] **R4 — Continuity/interoperability:** explicitly review underrun-tail policy,
  framing/format negotiation and configurable bench attenuation (T13/T15/T21).
  Underrun-tail policy changed to preserve queued PCM and resume immediately
  (startup still prebuffers); regression updated. Framing negotiation and
  configurable bench attenuation remain.
- [ ] **R5 — Timed sync:** schedule frame/timestamp controls only after presentation
  accounting and standalone playback are qualified (T19/T20).

## Sendspin player — P0/P1

Plan: [docs/sendspin-plan.md](docs/sendspin-plan.md). Captured protocol:
[docs/bench/sendspin-capture-2026-09-13.md](docs/bench/sendspin-capture-2026-09-13.md).
MA 2.10.3 speaks an older cleartext revision (no Noise); the spec-current
encrypted revision is a separate, later target.

- [x] **S01 — Phase 0 capture and spec lock.** Capture a real MA session and pin
  versions. Completed: Upgrade headers, `client/hello`/`server/hello`,
  `client/state`, `client/time`/`server/time`, `group/update`, `stream/start`/
  `stream/end`, `client/goodbye`, and the `>Bq` binary Type-4 layout are recorded;
  probe tool at `tools/sendspin_probe.py`.
- [ ] **S02 — Phase 1 transport and core protocol.** WebSocket client over
  `hal_tcp`, bounded cleartext JSON encode/decode, binary dispatch, time-filter
  port. Test with captured vectors and a simulated server. Gate: native harness
  reaches an active `player@v1` session against real MA with the clock converging.
- [ ] **S03 — Phase 2 WROOM PCM proof.** `stream/start` -> PCM16 -> Q1.31 ->
  reservoir/graph/I2S; small buffer; hard sync only. Gate: audible PCM from MA.
- [ ] **S04 — Phase 3 WROVER/PSRAM sync quality.** PSRAM jitter buffer, drift
  correction, measured inter-device phase error.
- [ ] **S05 — Phase 4 spec-current encryption/pairing.** Noise `KKpsk2` via
  mbedTLS and pairing, gated on a future MA/spec revision.

## LMS/transport completeness — P1

- [ ] **T10 — Reference wire corpus.** Add independently documented HELO/STAT/strm/
  cont/RESP/audg vectors, short/coalesced/fragmented packets, malformed values and
  partial-send fault injection. Do not generate expected offsets from parser code.
- [ ] **T11 — Output lifecycle/status.** Implement stream/output thresholds,
  meaningful jiffies/buffer counters, output-start/underrun/drained notifications,
  song position and track completion. Verify next-track/gapless sequencing with
  two tracks where decode finishes well before the hardware drains.
- [ ] **T12 — Discovery and reconnect.** Configurable identity/server, bounded retry
  backoff, stream/control timeout/cancellation, connection replacement and restart
  recovery. Test server restart, cable/Wi-Fi loss and no resource growth.
- [ ] **T13 — HTTP/PCM interoperability.** Explicit supported HTTP/ICY/chunking/TLS
  policy, WAV/AIFF stream framing, codec detection and sample-format/rate capability
  agreement. Accept only advertised supported formats; test actual LMS conversions.
- [ ] **T14 — Additional codecs.** Add bounded worker adapters for FLAC first, then
  MP3/Opus according to scope. Pin/licence dependencies and measure stack/heap/CPU,
  corruption behavior and track changes on both host and MCU.
- [ ] **T15 — Playback controls.** ReplayGain/headroom policy, click-free volume and
  mute, balance, seek/skip and consistent pause/resume/output power behavior. Test
  controls during startup, pause, EOF and source replacement.
- [ ] **T16 — End-to-end reference regression runner.** Exercise real EAF executable
  against simulated peer plus ALSA capture, not only protocol callbacks. Compare
  stereo samples, gains, timing events and lifecycle snapshots. Keep optional live
  LMS tests opt-in and isolated to a dedicated player identity.

## DSP and synchronization — P1/P2

- [ ] **T17 — DSP product controls.** Parametric EQ design/configuration, safe
  coefficient updates, configurable pre-EQ headroom and stereo/2.1 topology tests.
  Stereo MAX/TAS hardware does not alone validate a third physical subwoofer output.
- [ ] **T18 — Numerical quality.** Extended-state biquads or measured justification,
  impulse/frequency/phase/noise-floor sweeps, saturation/stability checks and SIMD
  equivalence. Implement dither only at a defined precision-reduction boundary.
- [ ] **T19 — Clock/PTS model.** Separate source, system, device and presentation
  clocks; define timestamp wrap, pipeline latency and scheduled starts. Verify
  no claim of sample accuracy based on decoded/accepted frames alone.
- [ ] **T20 — Drift compensation and multi-room.** Bounded controller + ASRC and/or
  ESP32 APLL backend; ±100 ppm long simulations, clock-step and saturation tests,
  then measure <1 ms cross-device phase error on physical outputs.

## Later board and qualification — P1/P2

- [ ] **T21 — ALSA robustness.** Inject partial writes/EAGAIN/xrun/suspend/unplug and
  drain timeout; test pause-capable and fallback plugins. Reconcile presented-frame
  counters after discarded hardware frames. Keep hard-real-time claims separate.
- [ ] **T22 — WROVER-IE/N16R8 memory validation.** Confirm actual silicon and memory
  parts, enable PSRAM for the reservoir only after map/cache tests; verify internal
  DMA buffers. Run memory pressure and simultaneous Wi-Fi/BT stress. Do not infer
  compatibility solely from a reseller memory suffix or LouderESP32 similarity.
  The PSRAM profile (`psram.conf` + `psram.overlay`, 65536-frame reservoir from the
  external shared heap) builds with Wi-Fi enabled and DRAM0 at 74%; physical map,
  cache and stress validation on the actual module still required.
- [ ] **T23 — TAS5805M integration.** Obtain carrier schematic, power/PDN/I2C address
  and clock requirements. Add HAL control driver and board initialization sequence,
  mute/ramp/fault handling, register profile and reset/recovery tests. Verify I2S
  clock availability during configuration before enabling the amplifier.
- [ ] **T24 — Release qualification/CI.** CI for formatting/static checks, ASan/UBSan,
  TSan concurrency tests, feature-off/on builds, pinned Zephyr smoke/board compile,
  fuzz/property tests, 24-hour stress and archived GPIO/DMA timing results. Track
  test coverage by contract; document reproducible flashing and user controls.

## Explicit extended deliverables — P2, scope before implementation

- [ ] **T25 — AES67/Dante direction.** Define actual interoperability, licensing,
  PTP/network requirements and acceptance peers before implementing another source.
- [ ] **T26 — Other boards/LE Audio.** Separate nRF5340/STM32 port and LC3/LE Audio
  work from Classic A2DP. Require compatible controllers/codecs and physical tests.

## Closed in this audit

- [x] **F01** Guard graph-owned buffer metadata across node callbacks; silence
  recovery restores original storage. Fault-node regressions reproduce old behavior.
- [x] **F02** Avoid overflow in finite extreme biquad design inputs; UBSan regression.
- [x] **F03** Add portable source architecture guard and literal audg regression
  independent of field-builder code. The previous stereo-volume fix is confirmed.
