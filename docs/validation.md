# Native validation

## Repository architecture audit

Reviewed at baseline 7114eb3 after user confirmation of stereo playback. Reproduced
graph metadata-contract failure and a finite-input DSP NaN-to-int conversion under
UBSan; fixed both and added fault-node/extreme-parameter regressions. Added EOS
preservation coverage, a literal independent audg wire vector and a portable-source
architecture CTest guard. Full ASan/UBSan suite: 15/15 passed. Clang host/Zephyr
checks and native_sim smoke pass. Findings, limitations and pending tasks are in
`docs/audit.md` and `TASKS.md`; no ESP32 hardware result is claimed by this audit.


## LMS stereo volume wire-layout correction

Corrected audg gains to byte offsets 14 and 18 (previously 18 and 22). The old
parser used the right gain for the left channel and a trailing sequence field
for the right channel, muting it when that field was zero. The earlier fixture
incorrectly duplicated the parser offsets; it now packs the actual protocol
fields and tests both 22-byte payloads and payloads with trailing sequence data.
Distinct nonzero gains, mute, unity and ignored sequence values are covered.
The corrected fixture failed against the old parser; all 14 ASan/UBSan tests
pass with the fix. Physical stereo listening after this fix has not been rechecked.


## ALSA, LMS volume and Zephyr A2DP binding — 2026-09-11

- All 14 ASan/UBSan tests passed, including real ALSA null/file plugin capture,
  exact stereo gain/mute sample bytes, pause/resume, restart and player EOF drain.
- The simulated LMS peer validates fragmented audg packets, independent gains,
  mute, adjust-disabled unity, over-unity capping and malformed-length rejection.
- A 4,800-frame silent stereo WAV at 48 kHz opened and drained the system default
  ALSA device with zero reservoir underruns. This does not validate listening
  quality or precise presentation timing. ALSA version: 1.2.16.1.
- Zephyr native_sim smoke passes with the A2DP adapter's mock-registration callback
  tests. These verify pinned API compilation, codec rejection, receive gating,
  owned payload copies and discontinuity; no physical Bluetooth radio was used.
- Clang checks pass for 32 host and 22 Zephyr translation units. Linux also builds
  with ALSA disabled. External ALSA/codec internals are outside static checks.

See [Linux audio](linux-audio.md) and [Bluetooth binding](bluetooth.md) for usage
and remaining device/application integration work.


## Formatting and static checks — 2026-09-11

Clang 22.1.8: clang-format verification plus clang-tidy/clangd checks passed for
all 30 host and 20 Zephyr project translation units. Fixed implicit size
multiplication conversions, made protocol comparisons explicit and preserved
TCP implementation include order during formatting. Documented check exclusions
and toolchain argument adaptation in [development.md](development.md).
The 12-case ASan/UBSan suite passed after cleanup, and Zephyr smoke passed.
Reconstructed milestone archives also built independently: core 5/5 tests,
WAV player 9/9, and embedded transport 11/11 without the optional SBC dependency.


## Live LMS and output-owner harness — 2026-09-11

Connected the new `eaf_lms_play` executable to Lyrion 9.1.1 at
192.168.11.132:3483 (UI on 9000). Server discovery confirmed the isolated EAF
player. Received 44.1 kHz stereo PCM; the final stopped stream consumed 306,176
frames with zero reported underruns. JSON-RPC checks confirmed connected play,
stationary paused position across two observations, and connected resume.
The initial unsupported-pause disconnect motivated the new optional synchronous
pause callback. Native output acknowledges it at a block boundary and preserves
queued data and deadline pacing.

All 12 sanitizer regression cases passed with pause/resume protocol assertions.
Zephyr network/SBC smoke also passed after the callback change. The live harness
used timed null output; no audible output, precise server elapsed-position
agreement, volume behavior, track advancement or hardware timing was validated.


## LMS startup control and output reports — 2026-09-11

The socket integration test now exercises all four autostart modes, continuation,
ready/start release ordering, exactly one STMs per stream, and output buffer and
elapsed-time STAT fields from a simulated output-owner snapshot. PCM callbacks
assert neither startup gate is active. Malformed cont, metadata continuation and
scheduled starts fail cleanly. Pause/resume and real output timing remain outside
this test. The full 12-case sanitizer suite passed; the expanded rejection cases
also passed in a focused rerun. The updated Zephyr configuration passes its smoke.

## LMS transport and SBC decoding — 2026-09-11

- Host build with optional pinned OI SBC decoder: 12/12 CTest cases passed.
- Clang AddressSanitizer/UndefinedBehaviorSanitizer build: 12/12 passed with
  `UBSAN_OPTIONS=halt_on_error=1`. Negative signed shifts found in upstream SBC
  synthesis code were fixed in generated build-local sources before this run.
- A simulated SlimProto server and separate HTTP socket exercise fragmented
  commands/headers/body, exact 16/24/32-bit PCM conversion, partial consumer
  writes, concurrent timestamp pings, RESP and decode EOF. Truncated bodies,
  chunked transfer, compressed bodies and ICY metadata are rejected.
- Real SBC fixture decoding checks CRC rejection, sample count/format, nonzero
  output and repeated partial writes into a 64-frame reservoir. This is not a
  codec conformance or perceptual quality test.
- Zephyr v4.3.0 native_sim network/SBC configuration built and printed
  `EAF smoke PASS (0)`. Real SBC decoding produced 512 reservoir frames. The TCP
  adapter compiled; sockets were exercised on Linux, not on Zephyr. No physical
  Bluetooth radio, I2S DMA or live LMS server was tested.

Build instructions, dependency revision and limitations: [embedded.md](embedded.md).


## Embedded foundation — 2026-09-11

- Zephyr v4.3.0, `native_sim/native/64`, GCC host toolchain: built and ran with
  `CONFIG_HEAP_MEM_POOL_SIZE=0`; console printed `EAF smoke PASS (0)`.
- Exercised static worker/semaphore pools, reservoir/graph EOF, shared atomics,
  protocol ingress and I2S buffer ownership with a test device. Alternating
  successful/failed writes across ten restarts did not exhaust the four slabs.
- Host suite expanded to 10 cases. All passed with Clang strict warnings,
  ASan/UBSan/leak checks, and ThreadSanitizer. The new case covers SlimProto
  fragmentation/coalescing, malformed lengths and callback failure, stream-field
  parsing, HELO bytes, SBC payload copying, queue overflow/discontinuity and
  100,000 concurrent packets crossing cursor and sequence rollover.
- Zephyr upstream inline headers are treated as system headers for EAF's strict
  diagnostic flags; EAF source retains warnings as errors.

No live LMS session, phone pairing, compressed SBC decoding, MCU firmware flash,
physical codec output or DMA timing was tested. The simulator is a Zephyr-kernel
test; its I2S device is a mock. See [embedded setup](embedded.md) for scope.

## Playback increment — 2026-09-11

All nine CTest cases passed with GCC 16.2.1 strict warnings, Clang 22.1.8
AddressSanitizer/UndefinedBehaviorSanitizer/leak checks, and Clang ThreadSanitizer.
Sanitizer suites ran outside the tracing sandbox, as for the initial foundation.

New or extended checks:

- EOF publication, short/empty tracks, exact and partial final blocks, zero
  padding, write rejection after EOF, reset/reopen and stable repeated EOF.
- 16/24/32-bit signed PCM conversion at extrema, mono/stereo channel layout,
  seek/read position, malformed RIFF sizes/alignment, odd unknown-chunk padding,
  unsupported tags and injected reader failure without position advancement.
- 100,000 concurrent control commands with 64-bit payloads, FIFO integrity,
  full-queue rejection and cursor rollover.
- Player seek/restart (direct and queued), gain ownership at block boundaries,
  read-error propagation, joining a producer blocked inside a partial reservoir
  write, and rejecting watermark combinations that could stall prebuffering.
- Direct heap-call interception through real DSP and the final EOF graph path.
- Generated WAV files through the actual file HAL and `eaf_play`: 19-frame stereo
  PCM16, 257-frame mono PCM24 at 44.1 kHz, 20,000-frame stereo PCM32, and an empty
  track. Verified exact source-frame totals. Missing/truncated files, directories
  and FIFOs are rejected. Python 3 is needed only for this integration test.

The new paths have no reported sanitizer diagnostics in these runs. Source
draining was tested with the synchronous null sink; ALSA hardware-queue draining,
compressed decoding, bounded disk-read cancellation and DSP-tail rendering
remain outside this validation.

## Initial foundation — 2026-09-10

All five CTest cases passed in each configuration:

| Build | Compiler | Result |
| --- | --- | --- |
| Debug, strict warnings as errors | GCC 16.2.1 | 5/5 |
| Address + undefined behavior + leak sanitizers | Clang 22.1.8 | 5/5 |
| ThreadSanitizer | Clang 22.1.8 | 5/5 |

Coverage:

- Reservoir bounds, partial/full writes, prebuffering, recovery threshold,
  16-frame fade, partial-tail discard and unsigned cursor rollover.
- Concurrent transfer of 1,048,576 stereo frames, randomized producer chunk
  lengths, fixed 64-frame consumption, exact ordering and channel values.
- Q1.31 extrema, saturating biquad arithmetic, 80 Hz low-pass and LR4 crossover
  comparisons against double-precision reference equations, expansion bounds,
  and filter history reset. Maximum observed error: 4,240.1 Q31 counts for the
  low-pass and 8,201.3 for the crossover on the tested waveforms. These are
  selected regression cases, not a complete frequency/noise/stability analysis.
- Lifecycle guards, node initialization failure cleanup, stopped sample-rate
  reconfiguration, node error silence, acquired-buffer release and stage order.
- Direct heap-call interception during graph processing, including the real
  crossover, EQ and volume nodes. Shared-library internal allocations are not
  covered by linker wrapping.
- One-second threaded native demo: 48,000 committed frames, three output
  channels, zero observed underruns. Startup silence counts toward these frames.

The default GCC sanitizer build could not link because its libasan runtime was
missing. Clang supplied working sanitizer runtimes. Sanitizer tests ran outside
the execution sandbox because LeakSanitizer requires thread inspection that its
tracing restrictions prevented. No sanitizer diagnostics were reported in the
successful runs. Reproduce the Clang builds by adding
`-DCMAKE_C_COMPILER=/usr/bin/clang` to the README's sanitizer configure commands.

No hardware, Zephyr, real DAC, network protocol, drift synchronization,
hard-real-time deadline or long-duration audio-quality validation was performed.
