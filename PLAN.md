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
rate changes, volume and pause; physical association/playback remains pending.
The network stack's 64 KiB heap is an explicit platform allocation exception;
static image fit is not runtime memory/timing qualification. See the
[app guide](platform/esp32_lms/README.md) for build and bench steps.
