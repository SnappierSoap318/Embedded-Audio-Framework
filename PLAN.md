# Implementation plan

The v0.4 architecture is the product direction;
[architecture v0.5](docs/architecture-v0.5.md) defines current contracts. Build and validate it in these
increments; hardware and protocol claims require their own integration tests.

Pending tasks and acceptance gates are tracked in [TASKS.md](TASKS.md).
The repository audit is [docs/audit.md](docs/audit.md); board sequencing is in
[docs/hardware-roadmap.md](docs/hardware-roadmap.md). First hardware: WROOM with
two MAX98357 modules; later WROVER-IE/N16R8 and TAS5805M (exact variants pending).

## Current priority: embedded player, with Linux as a test harness

1. **Zephyr target path (started):** module/Kconfig build, bounded static thread
   and semaphore pools, shared C11 atomics, and stereo I2S TX adapter with four
   aligned static slab blocks. Zephyr 4.3.0 `native_sim/native/64` smoke exercises
   the real kernel/HAL and a test I2S device. Next: select physical board, codec,
   pins and DMA memory region; run actual I2S and measure scheduling margin.
2. **LMS endpoint (started):** bounded SlimProto parser, TCP HAL, HELO/STAT/RESP,
   HTTP raw PCM decoding and partial-write backpressure are implemented. Host
   socket tests exercise fragmented input, timestamp pings and malformed bodies.
   Autostart/cont gates and an output-snapshot status API are now implemented.
   Immediate pause/resume and a native output-owner harness now work against
   Lyrion 9.1.1 with raw PCM and null output. Next: embedded owner integration,
   track completion, automatic reconnect and FLAC. T02 startup ownership is fixed:
   the native audio worker is created and gated before pipeline START. ALSA output and LMS
   stereo volume/mute now work in the Linux harness.
3. **Bluetooth endpoint (started):** bounded SBC packet ingress and decode worker
   with discontinuity resets, CRC rejection and partial reservoir writes. Optional
   OI SBC decoding runs on Linux and Zephyr native_sim. Next: confirm controller,
   test the new fixed-rate Zephyr A2DP endpoint binding with a phone. Radio,
   pairing and LE Audio/LC3 remain unimplemented.
4. **Clock synchronization:** define PTS and clock domains, presentation scheduling,
   bounded PI control, ASRC/hardware-PLL backends and +/-100 ppm tests. Required
   before claiming LMS multi-room accuracy.
5. **Supporting playback:** native WAV source/player, EOF/seek/gain queue and
   host tests already work. FLAC 1.5.0, mpg123 1.32.10 and Opus 1.6 are visible
   through pkg-config. Native ALSA is optional listening/debug infrastructure,
   not a prerequisite for the embedded milestones. ALSA 1.2.16.1 is installed and the optional
   PCM output is implemented. Codec adapters and dither remain work.
6. **Hardware qualification:** board-specific codec, PSRAM/cache, I2S clocks and
   DMA validation; long-running network/Bluetooth stress, discontinuity tests,
   GPIO timing and multi-room phase measurements.

## Decisions resolving ambiguities in arch.md

- Use actual C11 atomic objects, never cast a volatile integer to an atomic.
  Require always-lock-free 32-bit atomics. Publication is release/acquire;
  one producer owns the write cursor and one consumer owns the read cursor.
- Cursor wrap uses unsigned subtraction with capacity <= UINT32_MAX/2 and a
  power-of-two capacity so storage indexing remains correct across wrap.
- Buffer capacity includes **sample slots**, independently of channel format.
  Crossover expands backward first, then filters forward to preserve both
  unread stereo samples and temporal IIR history.
- An insufficient block discards its remaining queued tail, emits a 16-frame
  ramp from the last played sample, then silence. UNDERRUN lasts one pull;
  PREBUFFERING resumes on the next pull and waits for the high watermark.
  This deliberately trades the short tail for deterministic block timing.
  Published source EOF is different: preserve all queued frames, bypass the
  startup watermark, zero-pad the final block, and mark DRAINED without an
  underrun. Graph processing returns EAF_EOF after that block commits.
- Low watermark is diagnostic; required pull size triggers underrun. The
  consumer signals producer backpressure below a separate low watermark.
- Nodes declare input/output formats during initialization. Configure validates
  each boundary; init failures unwind initialized nodes and sink resources.
  Configure failure leaves INITIALIZED after successful cleanup; failed cleanup
  leaves RECOVERY with references retained until cleanup succeeds.
- Configure/reset/deinit require both audio and producer callers to be quiescent.
  A single audio thread owns processing, DSP state and lifecycle transitions.
  The player accepts STOP, SEEK and master GAIN through a fixed 16-command SPSC
  queue; only the owner applies changes. It handles at most one command per
  block. Seek first stops output and joins the producer before resetting state.
  Thread resources may be recreated while stopped, before the next START.
- Use Q2.30 coefficients (feedback commonly exceeds one) and 64-bit accumulation
  with two guard bits. Current histories are Q1.31: extended-precision recursive
  state and SIMD optimization remain work before low-frequency precision claims.
- Linux's null sink simulates deadlines using absolute monotonic sleeps; it does
  not establish hard-real-time scheduling or validate physical DMA timing.
- Zephyr I2S circulates a fixed, preallocated slab because the driver owns and
  returns submitted blocks. This is bounded buffer ownership, with no heap or
  growing pool. Physical DMA reachability and cache behavior need board testing.
- Protocol parsing and Bluetooth ingress are separately testable groundwork;
  they do not advertise end-to-end network or radio playback.

## T03 sink lifecycle

Sink deinit returns an error. A failed release retains graph/node references in
RECOVERY; retry stop/deinit before replacing configuration. Partial init is tracked
separately from a startable sink. Failed START attempts DROP immediately. A failed
write or drain requires stop, never reprocessing the same partially consumed block.
The native worker must be joined before graph cleanup; failed joins retain handles.
EOS commit performs a bounded queue drain: ALSA one second, Zephyr I2S DRAIN plus
reclaiming all four slab blocks under a shared one-second deadline. STOP is DROP.
Slab reclamation proves driver ownership release, not physical amplifier timing;
T09 retains that board acceptance gate. ALSA write retry remains bounded at 250 ms.
ALSA close consumes its handle even when reporting an error, so cleanup clears that
handle while reporting the error; a subsequent deinit can complete graph release.
Host fault tests and the Zephyr mock exercise retry paths without claiming radio,
DMA or speaker-time validation.

## T01 architecture reconciliation

Architecture v0.5 is the current contract, with v0.4 retained as historical product
scope. The canonical finite-source example is compiled and run by `arch_example`.
Native output is selected through a common facade plus a CMake-selected ALSA/null
backend; native CLI feature branches and the public EAF_HAVE_ALSA define are gone.
Source guards now include public headers and native adapter/player translation
units. POSIX command-line bootstrap is an explicit harness exception. Compile and
test both backend configurations; third-party allocation, board deadlines and
physical presentation remain separate acceptance gates.

## T04 scheduling and wake contract

`hal_thread_create_with_options` adds decoder/audio roles, optional CPU index
(-1 means unrestricted), and an explicit Linux realtime request. Existing create
calls retain decoder defaults. Linux ordinary workers explicitly use SCHED_OTHER;
FIFO is opt-in with audio priority 20 and decoder 10, and permission/configuration
failure returns EAF_IO without running the callback or retaining an allocated handle.
The native LMS consumer now declares its audio role; its default remains ordinary
Linux scheduling. Thread attributes apply before entry, including CPU affinity.

Zephyr uses separate Kconfig audio/decoder preemptive priorities (3/5 by default),
asserting that audio outranks decoder and both are valid. Creation holds the thread
until optional CPU pinning succeeds; unsupported pinning returns EAF_UNSUPPORTED.
The legacy EAF_THREAD_PRIORITY setting is replaced by EAF_AUDIO_PRIORITY and
EAF_DECODER_PRIORITY. Board applications must assign their audio worker role;
creating a decoder does not change the priority of the application main thread.

Binary semaphore wakeups coalesce and require a caller-owned atomic predicate.
Linux now uses sem_clockwait with CLOCK_MONOTONIC (checked at configure time), with
one absolute deadline retained through EINTR. Zephyr uses uptime-based kernel
waits. Zero timeout polls; expiry returns EAF_TIMEOUT, distinct from invalid inputs
or OS errors. Give tolerates an uninitialized/null handle. Init/deinit require
quiescent callers, and deinit is forbidden while waiters exist. Deadlines bound the
requested blocking interval, not scheduler dispatch latency; join is still a
blocking teardown operation after cooperative worker cancellation.

Host regression covers coalescing, interrupted timeout, 5,000 semaphore handshakes,
actual allowed-CPU pinning and denied FIFO creation with both role priorities.
Zephyr smoke checks role priorities, pool reuse, timeout and optional affinity.
Hardware measurements under radio contention are required before closing T04.

## First physical WROOM boot

On 2026-09-12, the UART-only internal-RAM application in `platform/esp32_boot`
was built with Zephyr SDK 0.17.4, flashed and verified on ESP32-D0WD-V3 revision
3.1 with 4 MB flash. PSRAM, I2S and radios are disabled. The gated producer and
null-output graph processed 4096 frames with zero underruns, then printed four
PASS heartbeats over an 18-second reset/boot capture. See
[bench evidence](docs/bench/wroom-boot-2026-09-12.md) for toolchain pins, memory,
image hash and local backup paths. Physical DAC wiring and radio memory/timing
qualification remain separate next steps; this does not close T04/T05/T06.

## WROOM network bench app (T07 started)

`platform/esp32_lms` now provides a no-PSRAM Wi-Fi/DHCP + LMS PCM player with
ignored local credentials, the physical Wi-Fi MAC identity, timed reconnect,
32 KiB stereo reservoir and selectable I2S/timed-null output. The I2S adapter
supports acknowledged pause/drain/resume; mono expands to stereo and LMS gain
follows fixed −18 dB bench attenuation. Host regression exercises lifecycle,
rate changes, volume and pause. Physical Wi-Fi/DHCP and LMS registration passed
on 2026-09-12; playback remains pending. See the
[connection evidence](docs/bench/wroom-lms-2026-09-12.md).
The network stack's 64 KiB heap is an explicit platform allocation exception;
static image fit is not runtime memory/timing qualification. See the
[app guide](platform/esp32_lms/README.md) for build and bench steps.

## Wireless diagnostics and playback starvation investigation

The WROOM app now includes a bounded application-log history and read-only HTTP
viewer on port 80, with browser timeout/retry regression coverage. Output underruns
are transferred atomically to the transport-thread status log. Connection logs
identify LMS connection versus transport/command failure. Network socket budgets,
receive window and timeout options are explicit in the board app configuration.
Physical diagnostics previously observed a tiny 272-byte stream and later severely
slow source progression; neither result establishes an amplifier fault. Sustained
playback with the revised network budget remains a required gate. Synchronized
start/pause is unsupported, and the fixed −18 dB bench attenuation remains enabled.

## Reference streaming audit — 2026-09-13

The [streaming comparison](docs/streaming-audit-2026-09-13.md) pins EAF `e1b7e27`
and squeezelite-esp32 `1d542bd5`. It identifies a definite receive-loop throughput
ceiling for some accepted 96 kHz formats, small fixed startup reserve, omitted LMS
thresholds/status/lifecycle events and missing established-connection progress
recovery. It does not establish the cause of the physical 30-second dropout.
`TASKS.md` now prioritizes R1–R5: improve bounded intake and evidence first, then
buffer readiness and standalone lifecycle, before timed sync. Preserve portable
HAL boundaries, static ownership and checked teardown; reference buffer sizes and
ESP-IDF scheduling choices are not suitable defaults for no-PSRAM WROOM.
Five relevant existing sanitizer regressions passed during the audit; sustained
physical throughput and event-order coverage remain required implementation gates.

## R1 bounded receive pumping

The portable LMS client now exposes a progress-driven pump with step and elapsed
time admission budgets. The WROOM services control before every HTTP step, yields
one tick after a busy batch, and polls after 2 ms only when idle/backpressured.
The synchronous stream setup/pause callbacks remain separately bounded operations,
not covered by the batch admission deadline. No new stream ring or worker was added.
Session counters retain receive bytes, published frames, would-block/backpressure
and budget yields; the first failure stage/opcode survives cleanup. Output queue
minimum is sampled by the audio owner and exported atomically. Regression covers
all accepted PCM rates/widths/channel counts, fragmented input/output, zero-space
backpressure, control STOP fairness and first-error retention. Physical sustainable
throughput and long-play qualification still gate R1 completion.

## R2 bounded buffer readiness

The optional paired LMS `start_buffered`/`release` callbacks prepare a writable
reservoir with its worker held. The board converts both wire thresholds to source
frames, takes the maximum with 3072 preferred frames, and clamps to its 4096-frame
capacity. No new ring or thread is required. The same watermark governs reservoir
rebuffering. All autostart/cont modes retain their gates; short and empty EOF can
qualify readiness, and stop/pause are supported during prefill. Legacy `start`
callbacks retain their existing behavior (including the Linux player).

Regressions cover fragmented input, oversized requests, server-controlled release,
short/empty EOF, pause/resume and cancellation of the real held output worker.
Hardware jitter and long-play validation remain open; R3 lifecycle and timed sync
are not implemented by this change.

R2 validation: all 23 native ASan/UBSan tests passed; ThreadSanitizer passed
`board_output`, `board_log` and `lms_pump`. Clang checks passed 42 native and 19
Zephyr translation units. ESP32 I2S and null images built with PSRAM disabled.
These are build/software results; this stage has not been flashed or measured
on the amplifier bench.

## RX window and ingress ring — physical result 2026-09-13

Physical WROOM playback showed HTTP PCM intake stuck near window/RTT: the 8 KiB
TCP receive window plus a client that stopped reading on output backpressure
delivered only ~80–140 KB/s of the 176,400 B/s needed for 44.1 kHz/16-bit stereo,
with 198 underruns in one track. The fix has two parts. The board raises the TCP
receive window to 16 KiB with a matched RX pbuf pool. The portable LMS client now
owns a raw source-PCM ingress ring (`EAF_LMS_INGRESS_BYTES`, default 4 KiB, 16 KiB
on WROOM): the transport keeps draining the socket into the ring while the output
reservoir is full, so the server's TCP window stays open, and decode sources from
the ring. This is the audit S05 staging buffer, sized from measured starvation
rather than the reference's PSRAM-scale values.

After flashing, 44.1 kHz/16-bit stereo sustained 174–177 KB/s, `played_ms` tracked
wall time (+5000 ms per 5 s), and underruns stayed at 0 for ~50 s then reached 1
total over ~85 s (`failed=0`). Audible output was not independently verified and
the destructive underrun-tail policy plus long-play/reconnect gates remain (R4).
Evidence: docs/bench/wroom-lms-ingress-2026-09-13.md.

## Reservoir continuity and single-core scheduling — 2026-09-13

The reservoir no longer discards a short queued tail on underrun. A short pull
in STREAMING now copies the available frames, ramps the shortfall toward silence
from the last real sample, counts the underrun and resumes STREAMING on the next
pull; PREBUFFERING remains a startup-only gate on the high watermark. This is the
audit S07 direction: preserve partial PCM, pad output, avoid the previous
discard-plus-refill-to-watermark gap. The reservoir regression asserts the new
behavior (partial frames retained, immediate resume).

Scheduling isolation could not use two cores: Zephyr 4.3.0's ESP32 Wi-Fi driver
has `depends on !SMP`, so enabling `CONFIG_SMP` silently drops Wi-Fi. With
networking, isolation is single-core: `CONFIG_NET_TCP_WORKER_PRIO` is raised to 5
so the TCP worker no longer outranks EAF audio (3). The board keeps
`EAF_BOARD_AUDIO_CPU` / `EAF_BOARD_MAIN_CPU` and `k_thread_cpu_pin` scaffolding,
inert until a Zephyr Wi-Fi stack supports SMP. The WROVER/PSRAM buffer path
remains the way to ride out multi-second network stalls.

## PSRAM reservoir and board capability split — 2026-09-13

The output reservoir capacity is now `CONFIG_EAF_BOARD_RESERVOIR_FRAMES` (power of
two) instead of a hardcoded 4096, and `CONFIG_EAF_BOARD_USE_PSRAM` allocates its
storage from the ESP32 external-RAM shared heap (`shared_multi_heap_alloc`,
`SMH_REG_ATTR_EXTERNAL`) during `board_output_init`, before START. The portable
reservoir already takes caller-owned storage, so no core change was needed.

Two profiles build from the same app: WROOM (internal RAM, 4096 frames) and
WROVER-E/N16R8 (`psram.conf` + `psram.overlay`, 8 MB PSRAM, 65536-frame reservoir
about 1.5 s, external heap 2 MB). The larger reservoir also stops clamping the LMS
stream threshold (261120 B) that the WROOM profile has to cap at 92 ms. The
DevKitC board already selects the WROVER-E N4R8 SoC, so the PSRAM node exists and
the profile overlay re-enables it. WROVER build: DRAM0 74%, ext_ram_seg 2 MB,
Wi-Fi enabled; physical map/cache/stress validation remains T22.

## STAT stream fields regression — 2026-09-13

Reporting the raw ingress ring as STAT `stream_buffer_size`/`stream_buffer_fullness`
was a regression on the WROOM. The ring reads as 100% full whenever the output
reservoir is backpressured, and because STAT is sent once per second, LMS paused
the HTTP stream until the next report. The 93 ms reservoir then drained and the
output underran continuously (~26% intake, ~274 underruns in the first second
after release, then an LMS pause). The truthful monotonic `jiffies` field is
kept; the stream/output buffer fields are back to zero and the speculative
STMo/STMu emissions are removed, restoring TCP flow control. Reintroduce S04
only with a stream buffer large enough that fullness stays well below 100%
between reports (the WROVER/PSRAM profile) or far more frequent updates, and
validate on hardware before trusting it.
