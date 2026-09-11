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
