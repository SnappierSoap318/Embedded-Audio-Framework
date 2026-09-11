# Embedded Audio Framework

Embedded C11 audio framework targeting Zephyr, with Linux as its test harness.
Architecture: [arch.md](arch.md).
See [PLAN.md](PLAN.md) for implementation stages and resolved API ambiguities.
Recorded host test results are in [docs/validation.md](docs/validation.md).

Implemented: bounded SPSC Q1.31 reservoir with recovery hysteresis and fade,
static pipeline lifecycle with configure rollback, Q2.30 biquads, per-channel
volume, LR4 stereo-to-2.1 crossover, Linux threads and binary wake semaphore,
and a sink with aligned ping-pong buffers and absolute wall-clock pacing.
Playback now includes an asynchronous PCM WAV source, EOF draining, and a bounded
control queue for stop, seek and gain. See [playback API notes](docs/playback.md).

Build and run on Linux with CMake, a C11 compiler, pthreads and libm:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/eaf_demo
./build/eaf_play /path/to/file.wav
```

The demo runs a background synthetic PCM producer and the audio consumer for
one second (48,000 frames, 128 frames/block). Its static graph applies -6 dB
headroom, an 80 Hz crossover, a bypassed EQ and unity master volume. The null
sink discards audio; this demo does not play through a sound card.
`eaf_play` reads mono/stereo 16/24/32-bit PCM WAV files through the same timed
null sink, reports source frames consumed, and exits after the final block.
It also produces no audible output. Python 3, when available, enables an
additional CTest that generates files and exercises the executable.

```sh
cmake -S . -B build-asan -DEAF_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DEAF_THREAD_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

EAF targets use the architecture's strict warning flags with warnings as errors.
Optional third-party SBC sources use their own diagnostic policy.
Tests remain active in Release builds. The graph test intercepts direct linked
malloc/calloc/realloc/free calls while processing; it does not inspect internal
allocations in shared system libraries or establish a whole-process heap bound.

## Ownership and API contract

- All pipeline, reservoir, node context and sink sample memory is caller-owned.
  Initialize objects to zero before first use. Reservoir storage must hold
  `capacity * num_channels` samples; capacity must be a power of two.
- Exactly one producer calls `write`, and one consumer calls `pull`. A partial
  write leaves the remaining input with the producer. Level is an approximate
  diagnostic for a third observer, and an endpoint-safe backpressure predicate.
- The consumer exclusively owns DSP state, reservoir recovery state and counters.
  Read those counters after joining the consumer. Configure, reset and lifecycle
  operations require external serialization and a quiescent producer.
- Create Linux threads and semaphores before START. The demo gates its producer
  until START resets the reservoir. Join threads and destroy semaphores after
  stopping their work. The audio processing loop allocates no heap memory.
- `capacity_samples` describes actual backing storage, independent of the current
  format. Nodes may expand channel count but may not change sample rate or block
  length. Channels follow ascending mask-bit order. Node callbacks must preserve
  the sink's samples pointer and capacities and honor these bounds.
- Configure propagates formats through node initializers. A node's deinitializer
  must safely handle a partially failed initializer. On configuration failure,
  the graph returns to INITIALIZED and can be configured again.
- STOP clears filter history. START clears queued PCM, EOF and underrun counters.
  Producer EOF preserves the final partial block. The player owns stop/seek/gain
  messages; direct control-thread mutation of active DSP contexts is prohibited.
- Biquad coefficients use `[b0,b1,b2,a1,a2]`, Q2.30, with feedback subtracted.
  Configure coefficients while stopped; callers are responsible for stability
  of custom EQ coefficients. Filter design uses floating point during configure;
  sample processing uses integer arithmetic. Recursive histories currently use
  Q1.31 precision; see the plan before assuming 32x64 low-frequency performance.
- An error returned by `process` requires the caller to stop/recover. For a node
  failure the graph commits silence to release the acquired sink buffer. A sink
  acquire/commit failure must also permit release through its STOP implementation.

## Zephyr and network-player development

The Zephyr module, static HAL, I2S adapter and native_sim smoke are now available.
The TCP/HTTP raw-PCM LMS client, SBC ingress and decoder worker are shared
between Zephyr and host tests. Optional OI SBC decoding runs in native_sim.
`eaf_lms_play SERVER_IPV4 [seconds]` registers a test player with timed null output
for live-server PCM and immediate pause/resume testing. See [embedded setup and scope](docs/embedded.md).
`west.yml` pins Zephyr 4.3.0; the sample adds this repository as an extra module.

## Scope

Implemented paths include native file playback, Zephyr kernel/HAL integration,
and a stereo I2S adapter tested with a simulated device. Full LMS transport/HTTP
player integration, Bluetooth pairing/profile negotiation and LC3 decoding, compressed
file codecs, PLL/ASRC synchronization, ALSA and board-specific codec bring-up
remain unfinished. Neither native_sim nor host tests establish physical DMA
or multi-room timing guarantees.
