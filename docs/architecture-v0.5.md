# EAF architecture v0.5 — current contracts

This document supersedes the API examples and conflicting invariants in
[arch.md](../arch.md), which remains the historical product direction.
[PLAN.md](../PLAN.md) records implementation decisions; [TASKS.md](../TASKS.md)
defines completion gates. The primary target is a Zephyr ESP32 LMS/A2DP player.
Linux is a listening and regression harness. Neither native simulation nor passing
host tests establishes board readiness, real-time deadlines or multi-room sync.

## Boundaries and build selection

- `core/`: portable C11 algorithms, reservoir, graph, DSP and control queue.
- `apps/`: portable protocol, decoder and player logic through EAF interfaces.
- `include/eaf/`: public types/contracts, without OS/MCU SDK headers.
  The optional `eaf_sbc_oi.h` exposes the portable OI codec context for static sizing.
- `hal/`: OS, transport and device implementations. Portable third-party codec
  calls are isolated in selected decoder adapters: `apps/decoders/sbc_oi.c`
  calls OI SBC through its optional public adapter header. Generic BT decoder
  logic depends on the EAF codec interface, not OI.
- `platform/`: static topology and application ownership. Native CLI argument,
  address and signal handling is an explicit POSIX harness exception. Zephyr
  bootstrap and mock devices may include Zephyr headers. Neither exception allows
  OS dependencies in the portable core or application protocols.

CMake/Kconfig selects translation units for optional backends. Executable portable
C and native player/adapter C contain no feature-condition branches. The native
output facade links either `native_output_alsa.c` or `native_output_null.c` and
serves one output per executable. Device selection precedes configuration. Pause
and delay run on the audio owner. No-device selection uses the timed null sink;
requesting a device in a null-only build returns `EAF_UNSUPPORTED`.
`tests/test_architecture.py` checks portable code/public headers for forbidden SDK
includes and checks portable/native C for preprocessor feature branches. It is a
source guard, not a full dependency graph or a proof about third-party internals.

## Memory, PCM and ownership

Audio samples are interleaved signed Q1.31, with an explicit sample rate, channel
count and channel mask. Frame capacity and sample-slot capacity are independent:
a node expanding stereo to 2.1 needs space for three samples per frame. Nodes may
expand channel count but cannot change sample rate, replace sink storage, alter
capacities or change block length. Configure validates formats; processing checks
metadata at each node boundary. It cannot contain an arbitrary invalid write made
inside a defective node. ASRC, extended recursive precision and dither remain work.
Biquad coefficients use Q2.30, Q1.31 histories and guarded 64-bit arithmetic.

Topology, contexts, queues and PCM storage are caller-owned and outlive the graph.
Graph configuration borrows node/sink/reservoir pointers. Objects must be zero
initialized before their first init. There is one graph/process owner and one
reservoir producer. Owners may hand off only when the previous owner is quiescent.
LMS, Bluetooth and file playback must not concurrently produce into one reservoir.
A complete embedded source-switch coordinator remains T08 work.

EAF processing code must not call the heap after START. Linux worker/semaphore
creation may allocate before START; shutdown joins workers before releasing their
resources. Teardown is outside the processing interval. A gated worker is created
before START and touches no graph state until START completes its reservoir reset.
A failed join retains its handle and prevents cleanup/reset. Zephyr threads and
semaphores use fixed slots; I2S circulates four preallocated, aligned slab blocks.
Taking/returning those slots is the explicit fixed-pool exception: no heap growth.
Driver ownership transfers only after a successful write and returns through the
slab. Board DMA reachability, alignment/cache behavior and memory budgets still
need qualification. ALSA/libc/codec internals are outside the direct EAF heap-call
guard; Linux playback does not claim whole-process zero allocation or hard real time.

## Scheduling and wakeups

Workers declare AUDIO or DECODER through `hal_thread_create_with_options`; legacy
create means decoder, unrestricted CPU, ordinary host scheduling. Zephyr defaults
to preemptive priorities 3/5, checked at build time so audio outranks the decoder.
Linux uses explicit SCHED_OTHER by default. An opt-in realtime request maps audio
to FIFO 20 and decoder to FIFO 10; errors are reported, with no silent downgrade.
CPU -1 is unrestricted. Linux applies pthread affinity attributes before entry;
Zephyr pins the not-yet-started worker when CONFIG_SCHED_CPU_MASK is enabled, or
returns EAF_UNSUPPORTED. Owners must still create/gate workers before START.
Existing application main threads are not reprioritized by this API.

Semaphores are binary coalescing wake hints, not event counters. Waits use monotonic
time: zero polls, expiration returns EAF_TIMEOUT, and interruption retries preserve
the original deadline. Recheck an atomic predicate after wake. Init/deinit require
quiescence; never destroy a semaphore with waiters. Requested wait deadlines do not
bound dispatch latency or establish a real-time guarantee. Join remains blocking,
after cooperative cancellation; ESP32 wake/compute margin under Wi-Fi/BT contention
is still a T04 hardware gate.

## Reservoir and control semantics

The reservoir uses always-lock-free C11 32-bit atomics with release/acquire
publication. Capacity is a power of two and no greater than `UINT32_MAX / 2`;
unsigned cursor subtraction supports wrap. Only the producer writes samples and
publishes EOF; only the consumer owns pull state and playback counters. A partial
write leaves the unwritten tail with the producer. Reset requires both endpoints
to be quiescent, including on track/source changes.

PREBUFFERING emits silence until the high watermark. A short non-EOF pull discards
its queued tail, emits a 16-frame ramp from the last output sample, then silence.
UNDERRUN lasts one pull; the next pull re-enters PREBUFFERING. Required block size
triggers underrun; producer backpressure has separate low/high watermarks and a
wake callback. No reader may treat consumer-owned counters as an atomic snapshot.

EOF is published after the last successful write. It bypasses the startup
watermark, preserves the remaining samples, zero-pads the final block and marks
DRAINED without an underrun. A successful EOS commit makes graph process return
`EAF_EOF`; later calls return EOF without committing more audio. Queue drain is not
an amplifier presentation timestamp. The control queue is bounded SPSC; the file
player applies at most one command per block. Native LMS gains use a bounded
coherent atomic mailbox read, with a contested update deferred to the next block.

## Lifecycle and failures

Normal flow is UNINITIALIZED → INITIALIZED → CONFIGURED → RUNNING → STOPPED.
STOPPED permits START with the same configuration or configure for a new one.
Lifecycle calls must be serialized with processing; reset/configure/deinit also
require the source producer to be quiescent. A control owner may configure before
releasing the audio worker and clean up after joining it.

Configure initializes nodes then the sink. Partial sink init is tracked even when
init returns an error. Cleanup calls sink deinit before releasing nodes; successful
rollback leaves INITIALIZED. Sink deinit now returns `int` (an API change from
v0.4). If it fails, graph retains references and enters RECOVERY. Only stop/deinit
retry is allowed there. Failed deinit invalidates the startable configuration;
a successful stop after that leaves INITIALIZED, requiring configure before START.
A cleanup implementation may have already released some resources, so retry must
be idempotent and must never reuse a consumed handle.

START failure immediately attempts STOP/DROP, including when START partly enabled
hardware. Failed DROP retains ownership. Successful rollback of a configured sink
leaves STOPPED; failed rollback leaves RECOVERY. Failed writes, invalid buffers and
drain errors require the owner to stop. Do not retry process as though the partially
consumed block were intact. The graph does not automatically stop a running worker.
Node failures restore trusted buffer metadata and attempt a silence commit before
returning the error. Deinit of a RUNNING graph is rejected.

STOP is immediate DROP, reclaiming outstanding acquired/queued storage only after
the driver accepts the stop. Successful EOS commit instead drains the driver queue:
ALSA has a one-second drain deadline and 250 ms write-retry deadline; Zephyr issues
I2S DRAIN and reclaims all four slab slots with one shared one-second deadline.
Timeout is an error, followed by owner-driven DROP recovery. These are bounded
software waits, not audio-period execution guarantees. Physical FIFO/amp tail,
underflow races and timing still need board tests under T09/T21. ALSA close consumes
its handle even on error; the adapter clears it while reporting failure so graph
cleanup can safely retry ([ALSA implementation](https://github.com/alsa-project/alsa-lib/blob/master/src/pcm/pcm.c)).

## Executable examples and validation

[tests/test_arch_example.c](../tests/test_arch_example.c) is the canonical minimal
finite-source example: static graph, configure/START, publish one stereo frame and
EOF, check padded output, STOP and checked deinit. CMake builds it and CTest runs
`arch_example`; there is no separately maintained pseudocode API. Its CHECK macro
is test-only fail-fast behavior. Production owners must retain state and report or
retry cleanup errors as demonstrated by the native LMS startup tests.
[platform/native_linux/board_config.c](../platform/native_linux/board_config.c)
is the larger static DSP graph with a gated asynchronous producer.

Validation combines graph/heap guards, SPSC stress, lifecycle fault injection,
ALSA capture, LMS socket peers, codec fixtures and Zephyr kernel/mock-driver smoke.
ASan/UBSan and separate TSan builds exercise memory and ownership properties.
Compile and test with ALSA enabled and explicitly disabled to verify adapter
selection. See [development.md](development.md) for commands. Completion still
requires the board, protocol, clock and endurance gates listed in TASKS.md.
