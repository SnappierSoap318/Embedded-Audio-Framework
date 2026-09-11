# Embedded Audio Framework (EAF) — Architecture Specification v0.4

Target Platforms: **Zephyr RTOS** (ESP32, nRF5340, STM32) and **Standalone Linux** (x86_64 / aarch64, pthreads, ALSA).
Primary Objective: Provide a deterministic, lock-free, zero-heap (post-init) embedded audio pipeline capable of running networked audio players (e.g., Logitech Media Server / Squeezelite multi-room streaming, Bluetooth A2DP, Dante/AES67) with sub-millisecond multi-room synchronization, low-latency DSP (EQ, 2.1 crossover, volume), and platform-agnostic portability.

---

## 1. Architectural Philosophy & Hard Invariants

1. **Zero Heap on the Audio Paths**: No calls to `malloc`, `free`, `k_malloc`, or dynamic pool allocations are permitted after the pipeline completes the `START` lifecycle transition.
2. **Strict Layer Decoupling**:
   * `core/` contains pure algorithmic, data structure, and state-machine logic. It must never include `<zephyr/...>`, `<pthread.h>`, or vendor MCU SDK headers.
   * `hal/` is the sole boundary where operating system and hardware SDK APIs are permitted.
   * `platform/` instantiates concrete, compile-time static topology graphs for specific boards.
3. **No `#ifdef` in Pipeline Logic**: Optional features (e.g., Bluetooth, decoders, ALSA vs. I2S) are toggled strictly at the build-system level by selecting between real and null translation units (`feature.c` vs `feature_null.c`).
4. **Canonical Audio Representation**: Internally, all audio on the processing and DMA path is **Signed 32-bit Fixed Point (Q1.31), Interleaved**, with explicit channel masks.
5. **Separation of Compute Concerns**: Bursty, non-deterministic decode operations run asynchronously from deterministic, deadline-driven DMA feed loops.

---

## 2. Repository Layout

```
eaf/
├── CMakeLists.txt                  # Universal root build (handles Zephyr or standalone Linux)
├── zephyr/
│   └── module.yml                  # Zephyr module definition
├── include/
│   └── eaf/
│       ├── eaf_types.h             # Formats, channel masks, frame structures
│       ├── eaf_core.h              # Node, Graph, and Lifecycle definitions
│       ├── eaf_reservoir.h         # Lock-free SPSC PCM ring buffer & hysteresis
│       ├── eaf_dsp.h               # In-place DSP interfaces (EQ, Volume, Crossover)
│       └── eaf_hal.h               # Abstract HAL interface (OS & Sink primitives)
├── core/
│   ├── eaf_graph.c                 # Static graph runner and lifecycle state machine
│   ├── eaf_reservoir.c             # SPSC Reservoir implementation
│   └── eaf_dsp.c                   # SIMD/intrinsics-accelerated in-place Q1.31 DSP
├── hal/
│   ├── hal_common.h                # Internal HAL types
│   ├── zephyr/
│   │   ├── hal_os_zephyr.c         # k_thread, k_sem, k_mutex, k_uptime
│   │   ├── hal_atomic_zephyr.c     # sys_atomic-backed atomic wrappers
│   │   └── sinks/
│   │       ├── sink_i2s_zephyr.c   # Generic Zephyr drivers/i2s.h sink
│   │       └── sink_i2s_esp32.c    # Low-level ESP32 I2S + APLL hardware clock tuning
│   └── linux/
│       ├── hal_os_linux.c          # pthreads, sem_t, clock_gettime
│       ├── hal_atomic_linux.c      # C11 stdatomic-backed wrappers
│       └── sinks/
│           ├── sink_alsa.c         # Linux ALSA output (for listening dev)
│           └── sink_null.c         # Headless wall-clock rate-limiting sink (for CI)
├── apps/
│   ├── lms/                        # SlimProto / Squeezelite protocol client
│   │   ├── lms_client.c
│   │   └── lms_client_null.c
│   ├── decoders/                   # Background stream decoders
│   │   ├── dec_flac.c
│   │   ├── dec_mp3.c
│   │   └── dec_opus.c
│   └── bt/                         # Bluetooth A2DP Sink
│       ├── bt_a2dp.c
│       └── bt_a2dp_null.c
└── platform/
    ├── native_linux/
    │   └── board_config.c          # Linux static pipeline configuration
    └── boards/
        ├── esp32_audio_kit.c       # ESP32 + PSRAM + ES8388 I2S codec board
        └── nrf5340_audio.c         # nRF5340 + Cirrus CS47L63 board
```

---

## 3. Data Representation & Math Conventions

### 3.1 Canonical Sample Format: Q1.31
All internal pipeline processing is standard signed 32-bit fixed point:
* $0\text{ dBFS} = \text{0x7FFFFFFF}$ ($+1.0 - 2^{-31}$)
* Minimum = $\text{0x80000000}$ ($-1.0$)
* 16-bit PCM from decoders is shifted left by 16 bits (`sample << 16`) at the reservoir input boundary.
* 24-bit PCM is shifted left by 8 bits (`sample << 8`).

### 3.2 Channel Layout & Masking
Frames are interleaved. Speaker channel positions are identified via an explicit bitmask:

```c
typedef enum {
    EAF_CH_FRONT_LEFT    = (1u << 0),
    EAF_CH_FRONT_RIGHT   = (1u << 1),
    EAF_CH_LFE           = (1u << 2), /* Subwoofer */
    EAF_CH_FRONT_CENTER  = (1u << 3),
} eaf_channel_mask_t;

typedef struct {
    uint32_t           sample_rate;   /* 44100, 48000, 96000 */
    uint8_t            num_channels;  /* 1 (Mono), 2 (Stereo), 3 (2.1), 4 (3.1/Quad) */
    eaf_channel_mask_t channel_mask;
} eaf_format_t;

typedef struct {
    int32_t *samples;                 /* Pointer to interleaved Q1.31 data */
    uint32_t frame_count;             /* Number of multi-channel frames */
    uint32_t capacity_frames;         /* Capacity of the allocated memory */
    eaf_format_t format;
    uint32_t flags;                   /* EAF_FRAME_FLAG_* */
} eaf_buffer_t;
```

### 3.3 DSP Headroom & Biquad Convention
To prevent digital clipping on positive EQ gain (boost) and eliminate quantization noise in low-frequency biquads (e.g. 50 Hz shelving):
1. **64-bit Accumulation**: All IIR/Biquad operations must compute using 32-bit coefficients and 64-bit accumulators (CMSIS Direct Form 1 `32x64` or Xtensa `MUL.AA.LL`).
2. **Headroom Attenuation**: The pipeline provides an optional configurable digital attenuation stage (default: $-6\text{ dB}$) prior to EQ, allowing up to $+6\text{ dB}$ of boost without hard clipping.
3. **Saturation**: All fixed-point additions and shifts must saturate, never overflow/wrap around (using `__QADD` / `__SSAT` intrinsics).

---

## 4. Execution & Threading Architecture

EAF isolates CPU-intensive, variable-duration workloads from the zero-jitter real-time output loop via a **two-thread + control thread** model.

```
                      CONTROL PLANE THREAD
               [CLI, Settings, LMS Metadata, RPC]
                               │
               (k_msgq / non-blocking events)
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│ Thread 1: Decode & Transport Task (Core 0 / Normal Priority)│
│ - Reads network sockets (HTTP/LMS), BLE packets, or files.  │
│ - Runs compressed format decoders (FLAC, MP3, Opus).        │
│ - Normalizes input to Interleaved Q1.31.                    │
│ - Writes variable frame chunks to the PCM Reservoir.        │
│ - Suspends via semaphore on high-watermark (backpressure).  │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               │ Lock-Free SPSC Writes
                               ▼
 ┌───────────────────────────────────────────────────────────┐
 │               PCM RESERVOIR (Ring Buffer)                 │
 │   - Sized for 200ms–500ms of Q1.31 audio                  │
 │   - Placed in external RAM (ESP32 PSRAM) or fast SRAM     │
 │   - Managed by 3-State Hysteresis Machine                 │
 └─────────────────────────────┬─────────────────────────────┘
                               │
                               │ Non-Blocking Atomic Reads
                               ▼
┌─────────────────────────────────────────────────────────────┐
│ Thread 2: Real-Time Audio Task (Core 1 / Max RTOS Priority) │
│ - Woken by I2S DMA Half/Full Transfer Interrupts.           │
│ - Owns DMA ping-pong buffers located in internal DMA-SRAM.  │
│ - Pulls fixed N frames from Reservoir.                      │
│ - Executes DSP stages sequentially IN-PLACE (Hot in L1):    │
│     1. Crossover / Channel Expansion (2.0 -> 2.1)           │
│     2. Multi-band Parametric EQ                             │
│     3. Master Volume & Balance                              │
│     4. TPDF Dither & Saturation Clamp                       │
│ - Commits aligned buffer directly to DMA engine.            │
└─────────────────────────────────────────────────────────────┘
```

---

## 5. Memory Model: Zero-Copy In-Place Pull Architecture

### 5.1 Memory Domain Partitioning
* **External PSRAM / Large Buffers**: The PCM Reservoir can be allocated in high-latency external memory because it is written in large linear bursts by Thread 1 and read linearly by Thread 2.
* **Internal SRAM / DMA Buffers**: All DMA ping-pong buffers and the active processing buffer **must** reside in fast internal SRAM, meeting hardware cache-line (32-byte) alignment and DMA memory-domain requirements.

### 5.2 The Pull Contract: Sink-Owned Buffers
The sink (which controls the hardware DMA descriptor) passes a reference to its own pre-allocated, aligned buffer upstream. Intermediate nodes process the buffer **in-place**, eliminating intermediate allocations and copies:

```c
/* Standard processing node execution callback */
typedef int (*eaf_node_process_fn)(void *ctx, eaf_buffer_t *buf);
```

### 5.3 Step-by-Step Cycle for a 128-Frame DMA Block
1. DMA triggers a transfer-complete interrupt; Thread 2 wakes up.
2. Sink passes its inactive ping-pong buffer (`eaf_buffer_t`) to the graph runner.
3. The graph runner queries the **PCM Reservoir**:
   * If the reservoir is `STREAMING`, it copies 128 frames of Q1.31 PCM directly into the sink buffer.
   * If the reservoir is `PREBUFFERING` or `UNDERRUN`, it zeroes the sink buffer (producing silence) and transitions state without blocking.
4. The graph runner passes the sink buffer sequentially through the configured nodes:
   * `eq_node->process(eq_ctx, buf)`: Mutates `buf->samples` in-place within L1 cache.
   * `vol_node->process(vol_ctx, buf)`: Mutates `buf->samples` in-place within L1 cache.
5. The sink flushes D-cache if necessary and commits the buffer to the DMA controller.

---

## 6. Reservoir Primitive & Hysteresis State Machine

To prevent buffer thrashing (rapid toggling between audio and silence that sounds like a loud buzz) when network jitter occurs, the reservoir implements a strict three-state hysteresis automaton.

```
                     ┌──────────────────┐
       Init / Reset  │                  │
      ──────────────>│  PREBUFFERING    │<─────────────────┐
                     │ (Outputs Silence)│                  │
                     └────────┬─────────┘                  │
                              │                            │
             Fill Level >= High Watermark                  │
             (e.g., 70% capacity)                          │
                              │                            │
                              ▼                            │
                     ┌──────────────────┐                  │
                     │    STREAMING     │                  │
                     │  (Outputs Audio) │                  │
                     └────────┬─────────┘                  │
                              │                            │
             Fill Level < Low Watermark                    │
             (e.g., 0 frames remaining)                    │
                              │                            │
                              ▼                            │
                     ┌──────────────────┐                  │
                     │     UNDERRUN     │                  │
                     │ (Mute/Fade Out)  │──────────────────┘
                     └──────────────────┘
```

### 6.1 State Definitions
* **`EAF_RESERVOIR_PREBUFFERING`**: The reservoir is accumulating frames from the decode thread. Pull requests on the real-time thread return `0` frames (the sink fills its buffer with zeros). No audio is emitted.
* **`EAF_RESERVOIR_STREAMING`**: The fill level has reached `high_watermark`. The real-time thread pulls $N$ frames continuously.
* **`EAF_RESERVOIR_UNDERRUN`**: The fill level dropped below the required pull size. The reservoir immediately applies a micro-fade-out (16 samples) to prevent a DC click, records an underrun counter, and drops into `EAF_RESERVOIR_PREBUFFERING`. It will not output audio again until `high_watermark` is fully reached.

### 6.2 Backpressure Signaling
* When the reservoir level drops below `backpressure_low_watermark` (e.g., 50% capacity), it signals a HAL semaphore to wake up the Decode Thread.
* When the reservoir reaches `backpressure_high_watermark` (e.g., 90% capacity), the Decode Thread blocks on that semaphore, pausing network reads and decoder computation.

---

## 7. Multi-Channel Crossover Pipeline (2.0 $\to$ 2.1 / 3.1)

To handle modern 2.1 systems (Stereo speakers + Subwoofer) commonly used in DIY and commercial streaming appliances, EAF incorporates a dedicated Channel Expansion & Crossover Node:

```
                  2-Channel Input [L, R]
                            │
               ┌────────────┴────────────┐
               ▼                         ▼
      High-Pass Biquad          Summing Junction
       (e.g. 80 Hz HPF)         Mono = (L + R) / 2
               │                         │
               │                         ▼
               │                  Low-Pass Biquad
               │                  (e.g. 80 Hz LPF)
               ▼                         ▼
         [L_out, R_out]               [Sub_out]
               │                         │
               └────────────┬────────────┘
                            ▼
              3-Channel Interleaved Output
                  [L, R, Sub, L, R, Sub...]
```

* **Memory Handling**: The crossover stage reads from a 2-channel reservoir buffer and writes into a 3-channel (or 4-channel, if padded for 32-bit I2S slot alignment) sink buffer.
* **Phase Alignment**: 4th-order Linkwitz-Riley filters (-24 dB/octave) are implemented by cascading two identical 2nd-order Butterworth biquads. This ensures that the low-pass and high-pass outputs are strictly in-phase at the crossover frequency.

---

## 8. Clock Synchronization & Drift Compensation

In networked multi-room audio (LMS, Dante), remote transmitter crystals and local DAC crystals drift by 10–100 PPM. Without compensation, the local reservoir will experience catastrophic underflow or overflow within minutes.

EAF standardizes on a **dual-mode synchronization engine**:

```c
typedef enum {
    EAF_CLOCK_SYNC_NONE,
    EAF_CLOCK_SYNC_HARDWARE_PLL, /* Micro-slewing hardware audio PLL (ESP32 APLL) */
    EAF_CLOCK_SYNC_SOFTWARE_ASRC  /* In-line fractional polynomial resampler */
} eaf_clock_sync_mode_t;
```

### 8.1 Mode 1: Hardware Audio PLL Slew (ESP32 / Advanced MCUs)
* On platforms with tunable audio PLLs (e.g., ESP32 `rtc_clk_apll_enable()`, STM32 SAI PLL fractional multipliers), the framework computes drift against network presentation timestamps (PTS).
* It dynamically trims the peripheral clock frequency by adjusting fractional PLL dividers:
  $$\text{Target PPM} = \text{Clamp}\left(K_p \cdot \text{Error} + K_i \cdot \int \text{Error}, -300, +300\right)$$
* **Result**: Zero CPU overhead, no audio resampling artifacts, pristine audio quality, sub-millisecond multi-room sync.

### 8.2 Mode 2: Software ASRC (Generic Zephyr / Linux fallback)
* On platforms where the clock cannot be steered dynamically (standard Zephyr `drivers/i2s.h` or generic PC sound cards), an optional **Asynchronous Sample Rate Converter (ASRC)** node is placed immediately before the sink.
* The ASRC implements a low-overhead Farrow cubic or windowed sinc interpolator that dynamically adjusts its time step ratio based on the reservoir drift vector.

---

## 9. Hardware Abstraction Layer (HAL) Interfaces

All platform dependencies are confined to `hal/`. The following interface must be implemented for each supported target OS.

```c
/* include/eaf/eaf_hal.h */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* --- Operating System Primitives --- */
typedef struct eaf_thread eaf_thread_t;
typedef struct eaf_mutex  eaf_mutex_t;
typedef struct eaf_sem    eaf_sem_t;

int  hal_thread_create(eaf_thread_t *thread, const char *name,
                       void (*entry)(void *arg), void *arg,
                       int priority, size_t stack_size, int core_affinity);
void hal_thread_destroy(eaf_thread_t *thread);

int  hal_mutex_init(eaf_mutex_t *mutex);
int  hal_mutex_lock(eaf_mutex_t *mutex, uint32_t timeout_ms);
int  hal_mutex_unlock(eaf_mutex_t *mutex);

int  hal_sem_init(eaf_sem_t *sem, unsigned int initial_count, unsigned int max_count);
int  hal_sem_take(eaf_sem_t *sem, uint32_t timeout_ms);
void hal_sem_give(eaf_sem_t *sem);

uint64_t hal_monotonic_time_us(void);
void     hal_sleep_ms(uint32_t ms);

/* --- Atomic Primitives (Guaranteed lock-free across cores) --- */
typedef struct { volatile uint32_t value; } eaf_atomic_u32_t;

void     hal_atomic_set(eaf_atomic_u32_t *atomic, uint32_t val);
uint32_t hal_atomic_get(const eaf_atomic_u32_t *atomic);
uint32_t hal_atomic_add(eaf_atomic_u32_t *atomic, uint32_t val);
uint32_t hal_atomic_sub(eaf_atomic_u32_t *atomic, uint32_t val);
bool     hal_atomic_cas(eaf_atomic_u32_t *atomic, uint32_t expected, uint32_t desired);

/* --- Hardware Audio Sink Abstraction --- */
typedef struct eaf_sink eaf_sink_t;

struct eaf_sink_ops {
    int  (*init)      (eaf_sink_t *sink, const eaf_format_t *fmt, size_t buffer_frames);
    int  (*start)     (eaf_sink_t *sink);
    int  (*stop)      (eaf_sink_t *sink);
    int  (*acquire_buf)(eaf_sink_t *sink, eaf_buffer_t **buf);
    int  (*commit_buf) (eaf_sink_t *sink, eaf_buffer_t *buf);
    int  (*adjust_ppm)(eaf_sink_t *sink, int32_t ppm_delta);
    void (*deinit)    (eaf_sink_t *sink);
};

struct eaf_sink {
    const struct eaf_sink_ops *ops;
    void                      *driver_data;
};
```

---

## 10. Static Graph Instantiation

Instead of error-prone dynamic loading or linker-section discovery, pipelines are configured as explicit, compile-time static arrays in `platform/boards/<board>.c`.

### 10.1 Node Definition
```c
/* include/eaf/eaf_core.h */

typedef enum {
    EAF_NODE_STAGE_PRE_PROCESS,  /* e.g., Crossover / Channel Splitter */
    EAF_NODE_STAGE_DSP,          /* e.g., Equalizer, Bass Boost */
    EAF_NODE_STAGE_POST_PROCESS, /* e.g., Volume, Limiter, Dither */
} eaf_node_stage_t;

typedef struct eaf_node eaf_node_t;

struct eaf_node_ops {
    int  (*init)   (eaf_node_t *node, const eaf_format_t *fmt);
    int  (*process)(eaf_node_t *node, eaf_buffer_t *buf); /* In-place mutation */
    int  (*reset)  (eaf_node_t *node);
    void (*deinit) (eaf_node_t *node);
};

struct eaf_node {
    const char                 *name;
    eaf_node_stage_t            stage;
    const struct eaf_node_ops  *ops;
    void                       *ctx;
};

typedef struct {
    eaf_reservoir_t     *reservoir;
    const eaf_node_t   **nodes;
    size_t               node_count;
    eaf_sink_t          *sink;
} eaf_pipeline_config_t;
```

### 10.2 Concrete Board Wiring Example (ESP32 2.1 Board)
```c
/* platform/boards/esp32_audio_kit.c */
#include <eaf/eaf_core.h>
#include <eaf/eaf_reservoir.h>
#include <eaf/eaf_dsp.h>

/* Statically allocated memory blocks */
#define RESERVOIR_FRAMES (48000 / 2) /* 500ms of 48kHz stereo */
static EXT_RAM_BSS_ATTR int32_t s_reservoir_storage[RESERVOIR_FRAMES * 2];
static eaf_reservoir_t s_pcm_reservoir;

/* Node Contexts */
static eaf_crossover_2_1_ctx_t s_crossover_ctx;
static eaf_eq_ctx_t            s_eq_ctx;
static eaf_volume_ctx_t        s_volume_ctx;

/* Extern Sinks */
extern eaf_sink_t g_esp32_i2s_sink;

/* Node Definitions */
static const eaf_node_t node_crossover = {
    .name  = "crossover_2.1",
    .stage = EAF_NODE_STAGE_PRE_PROCESS,
    .ops   = &eaf_crossover_2_1_ops,
    .ctx   = &s_crossover_ctx
};

static const eaf_node_t node_eq = {
    .name  = "biquad_eq",
    .stage = EAF_NODE_STAGE_DSP,
    .ops   = &eaf_biquad_eq_ops,
    .ctx   = &s_eq_ctx
};

static const eaf_node_t node_volume = {
    .name  = "master_vol",
    .stage = EAF_NODE_STAGE_POST_PROCESS,
    .ops   = &eaf_volume_ops,
    .ctx   = &s_volume_ctx
};

static const eaf_node_t *s_board_nodes[] = {
    &node_crossover,
    &node_eq,
    &node_volume
};

const eaf_pipeline_config_t g_board_pipeline = {
    .reservoir  = &s_pcm_reservoir,
    .nodes      = s_board_nodes,
    .node_count = 3,
    .sink       = &g_esp32_i2s_sink
};

int board_pipeline_setup(void) {
    /* Initialize reservoir in PSRAM */
    eaf_reservoir_init(&s_pcm_reservoir, s_reservoir_storage, RESERVOIR_FRAMES, 2);
    /* Initialize the graph runner */
    return eaf_pipeline_init(&g_board_pipeline);
}
```

---

## 11. Lifecycle State Transition Rules

Every component (pipeline, node, sink, reservoir) strictly abides by these deterministic transitions:

```
[ UNINITIALIZED ] ──( init )──> [ INITIALIZED ] ──( configure )──> [ CONFIGURED ]
                                                                        │
                                                                     ( start )
                                                                        │
                                                                        ▼
[ STOPPED ] <──────( stop )────── [ RUNNING ] <────────────────────────┘
     │                                │
     └──( configure / format change )─┘
```

1. **`INIT`**: One-time allocation of static pointers, binding of context memory. No I/O or DMA activity.
2. **`CONFIGURE`**: Applies sample rate, channel mask, and buffer size. Validates that downstream sinks and upstream reservoirs match constraints. Can be invoked repeatedly while in `STOPPED` state to support track-to-track format changes (e.g. 44.1 kHz $\to$ 96 kHz).
3. **`START`**: Begins DMA hardware interrupts, starts decode/transport thread. Audio enters `PREBUFFERING`.
4. **`STOP`**: Parks the DMA controller, disables interrupts, suspends the decode thread. Node states (e.g. biquad filter histories) are cleared to prevent pops/clicks on resume.
5. **`DEINIT`**: Complete teardown (used primarily in Linux testing environments).

---

## 12. Build System & Toolchain Specification

### 12.1 Unconditional Clean Compilation
The build system mandates the elimination of `#ifdef` blocks within C logic for feature management. Instead, use translation unit substitution:

* **In CMake (Linux Standalone)**:
  ```cmake
  # CMakeLists.txt
  option(EAF_ENABLE_BT "Enable Bluetooth A2DP Support" OFF)

  if(EAF_ENABLE_BT)
      target_sources(eaf PRIVATE apps/bt/bt_a2dp.c)
      target_link_libraries(eaf PRIVATE bluez)
  else()
      target_sources(eaf PRIVATE apps/bt/bt_a2dp_null.c)
  endif()
  ```

* **In Zephyr (Kconfig / CMake)**:
  ```cmake
  # CMakeLists.txt
  zephyr_library_sources_ifdef(CONFIG_EAF_BT   apps/bt/bt_a2dp.c)
  zephyr_library_sources_ifndef(CONFIG_EAF_BT  apps/bt/bt_a2dp_null.c)
  ```

### 12.2 Warning Flags (Zero Tolerance)
All targets must compile cleanly with:
`-Wall -Wextra -Werror -Wconversion -Wstrict-prototypes -Wshadow -Wdouble-promotion`

---

## 13. Verification & Testing Matrix

### 13.1 Tier 1: Native Linux Host Unit Tests (Run in CI via `ctest`)
1. **`test_reservoir_spsc`**: Multi-threaded torture test verifying thread-safety of the SPSC lock-free reservoir under randomized write sizes and continuous fixed-size reads.
2. **`test_hysteresis`**: Injects artificial packet loss and network stutter; validates that underruns never emit audio frames with discontinuous gaps and that the reservoir strictly respects `high_watermark` transitions.
3. **`test_dsp_q31`**: Numerical precision test comparing output of `eaf_dsp` biquads and crossovers against double-precision 64-bit reference models computed on desktop Python/NumPy.
4. **`test_clock_slew`**: Simulates $\pm 100\text{ PPM}$ clock drift; validates that the PLL slewing algorithm / software ASRC recovers phase without clipping.

### 13.2 Tier 2: Zephyr `native_sim` Smoke Tests
1. Verifies that `hal/zephyr/` primitives (`k_thread`, `k_sem`, `sys_atomic`) cleanly compile, link, and run within Zephyr's simulated target environment.

### 13.3 Tier 3: Physical Hardware Benchmarks (ESP32, STM32, nRF5340)
1. **DMA Jitter Benchmark**: Measure GPIO pulse duration toggled inside the I2S transfer complete ISR; ensure RT audio thread processing latency consumes $< 40\%$ of the DMA buffer period.
2. **Cache Coherency Audit**: Verify non-cached allocation attributes on DMA buffers to guarantee no stale cache reads under continuous 24-hour stress tests.