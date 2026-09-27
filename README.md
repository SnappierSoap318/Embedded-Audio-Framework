# Embedded Audio Framework

[![CI](https://github.com/SnappierSoap318/Embedded-Audio-Framework/actions/workflows/ci.yml/badge.svg)](https://github.com/SnappierSoap318/Embedded-Audio-Framework/actions/workflows/ci.yml)
[![License: GPL-3.0](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

Embedded C11 audio framework with Zephyr and ESP-IDF backends, with Linux as its
test harness.
It provides a bounded SPSC audio reservoir, a static DSP pipeline, sink
adapters (I2S, ALSA, null), and network players built on a shared output owner:
a cleartext Sendspin player (Music Assistant) and a SlimProto/HTTP LMS client.

Licensed under the [GNU General Public License v3.0](LICENSE).

## Quick start (Linux host)

Requires CMake 3.20+, a C11 compiler, pthreads, libm, and glibc `sem_clockwait`.

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/eaf_demo
./build/eaf_play /path/to/file.wav
```

The demo runs a synthetic producer through the null sink (no sound card);
`eaf_play file.wav [device]` plays a WAV file and uses ALSA when a device is
given and ALSA is installed. Sanitizer builds:

```sh
cmake -S . -B build-asan -DEAF_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DEAF_THREAD_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure
```

## Documentation

| Area | Documents |
|---|---|
| Design contracts | [architecture v0.5](docs/architecture-v0.5.md), [historical product direction](arch.md), [audit](docs/audit.md) |
| Plan and status | [PLAN.md](PLAN.md), [TASKS.md](TASKS.md), [validation results](docs/validation.md) |
| Build, test, lint | [development workflow](docs/development.md), [Zephyr setup](docs/zephyr-setup.md) |
| Embedded/Zephyr | [embedded scope](docs/embedded.md), [hardware roadmap](docs/hardware-roadmap.md), [Bluetooth](docs/bluetooth.md) |
| Players | [Sendspin plan](docs/sendspin-plan.md), [playback API](docs/playback.md), [Linux audio](docs/linux-audio.md) |
| Reviews | [streaming audit 2026-09-13](docs/streaming-audit-2026-09-13.md) |
| Contributing | [CONTRIBUTING.md](CONTRIBUTING.md), [license (GPL-3.0)](LICENSE) |

### Board applications

| App | Board | README |
|---|---|---|
| Sendspin player | ESP32 (WROOM, WROVER + TAS5805M) | [platform/esp32_sendspin](platform/esp32_sendspin/README.md) |
| LMS player | ESP32-WROOM + MAX98357 | [platform/esp32_lms](platform/esp32_lms/README.md) |
| Boot/echo bring-up | ESP32 | [platform/esp32_boot](platform/esp32_boot/README.md) |
| ESP-IDF v6 HAL/I2S smoke target | Classic ESP32 (WROVER pin defaults) | [Build instructions](docs/development.md#esp-idf-backend-and-smoke-target) |
| Bluetooth A2DP SBC speaker | Classic ESP32 + TAS5805M (ESP-IDF) | [Bluetooth bindings](docs/bluetooth.md) |

### Bench reports

- [Sendspin Phase 0 capture (MA 2.10.3)](docs/bench/sendspin-capture-2026-09-13.md)
- [WROVER I2S isolation and track-change fix](docs/bench/wrover-i2s-2026-09-20.md)
- [WROOM boot](docs/bench/wroom-boot-2026-09-12.md), [LMS registration](docs/bench/wroom-lms-2026-09-12.md),
  [ingress ring](docs/bench/wroom-lms-ingress-2026-09-13.md), [wireless diagnostics](docs/bench/wroom-wireless-diagnostics-2026-09-12.md),
  [MAX98357A wiring](docs/bench/wroom-max98357a-wiring.md)

## Status

Hardware I2S is under investigation on the ESP-IDF backend: a clean
single-reader capture shows the console becomes unreadable during I2S init,
while the same image restricted to the HAL semaphore/thread checks stays clean.
Physical audio output and DMA behavior are not currently validated. The
[I2S bench report](docs/bench/wrover-i2s-2026-09-20.md) is historical context,
not evidence that the current hardware path passes.

`platform/esp_idf_bt` adds a Classic Bluetooth A2DP SBC sink: a Bluedroid
binding (external-codec mode) republishes undecoded SBC into the portable
ingress and drives the vendored OI decoder into the I2S sink. It builds clean
under ESP-IDF v6.0.2; pairing and playback against a real source are pending.

`TASKS.md` is the authoritative remaining-work list. In brief: release
qualification and recovery, restored OTA, board codec integration and resource
measurements, Bluetooth hardware validation and AAC/LDAC, DSP controls, and
synchronized multi-room playback.

### Codecs

- The decoder/worker layer implements PCM, FLAC (`dr_flac`), MP3 (`dr_mp3`),
  raw-packet Opus (fixed-point `libopus`) and Vorbis (`stb_vorbis`), with host
  tests. The native `eaf_play` command remains WAV-only.
- Sendspin negotiates configurable codec preferences and supports caller-owned
  decoder adapters. Live incremental FLAC/MP3 needs an `await` hook serviced by
  a decode thread; inline decoding currently stages the whole stream.
  Opus/Vorbis support incremental decoding. Board availability depends on the
  selected profile and its adapter wiring.
- The ESP-IDF smoke manifest includes PCM and FLAC only. Codec implementation
  and host tests do not establish MCU stack, heap, CPU or playback qualification.

## Building a board application

Board apps compile the shared `platform/esp32_output` owner against a selected
sink and storage provider. The server IPv4 address is a Kconfig option
(`CONFIG_EAF_BOARD_SERVER`) with an empty default; the player name and client id
are `CONFIG_EAF_BOARD_NAME` / `CONFIG_EAF_BOARD_CLIENT_ID`. Provide local values
without touching tracked files:

```sh
# platform/esp32_sendspin/local.conf (untracked)
CONFIG_EAF_BOARD_SERVER="192.168.1.10"
CONFIG_EAF_BOARD_NAME="Living Room"
```

Then pass it alongside the profile confs:

```sh
cmake -S platform/esp32_sendspin -B build-sendspin -G Ninja \
  -DBOARD=esp32_devkitc/esp32/procpu -DZEPHYR_TOOLCHAIN_VARIANT=zephyr \
  -DEXTRA_CONF_FILE=psram.conf\;local.conf \
  -DPython3_EXECUTABLE=/path/to/python \
  "-DZEPHYR_MODULES=$PWD;/path/to/hal-espressif;/path/to/mbedtls"
```

See the [Sendspin board README](platform/esp32_sendspin/README.md) for
credentials, PSRAM profiles, flashing and OTA.

### ESP-IDF v6 smoke target

`platform/esp_idf_sendspin` is a standalone HAL/I2S smoke project, not yet a
networked Sendspin board player. Its component manifest selects portable code
and the FreeRTOS/lwIP/`driver/i2s_std.h` backend independently of the root CMake
build. Activate an ESP-IDF v6.x environment (locally tested: v6.0.2), then run
from the repository root:

```sh
idf.py -C platform/esp_idf_sendspin set-target esp32
idf.py -C platform/esp_idf_sendspin build
```

The current smoke entry point runs the HAL semaphore/thread/monotonic-clock
checks and then plays a bounded 440 Hz I2S tone. A single-reader capture shows
the console surviving the HAL checks but becoming unreadable during I2S
initialization, so hardware I2S is under investigation. Default pins are WS=25,
BCLK=26 and DOUT=22; see the
[development workflow](docs/development.md#esp-idf-backend-and-smoke-target)
for configuration and toolchain details. Shared board-output integration is
still pending.

### ESP-IDF Bluetooth A2DP sink

`platform/esp_idf_bt` is a standalone Classic Bluetooth A2DP SBC speaker
scaffold. It enables Bluedroid Classic with `CONFIG_BT_A2DP_USE_EXTERNAL_CODEC`
so the stack hands over undecoded SBC frames, decodes them with the vendored OI
decoder and drains a jitter reservoir into the ESP-IDF I2S sink. It builds with:

```sh
idf.py -C platform/esp_idf_bt set-target esp32
idf.py -C platform/esp_idf_bt build
```

The device name and pins are configurable in
`platform/esp_idf_bt/components/main/Kconfig.projbuild`. Phone pairing and audible
SBC playback on ESP32/TAS5805M are user-confirmed. The app adds AVRCP volume/mute,
track metadata, reconnect/pairing-window management, forget/remove-bond, a
discontinuity-based stream-boundary handling and a UART control console. A
separate decode worker keeps decoding off the I2S output path to avoid stutter.
Source arbitration and Wi-Fi/BT coexistence remain T08 work; the new controls
await hardware testing.

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

## Repository layout

- `core/`, `include/eaf/` — reservoir, graph, DSP, control and public headers.
- `apps/` — WAV/PCM/FLAC/MP3/Opus/Vorbis decoders, player, LMS client,
  Bluetooth ingress/SBC, Sendspin.
- `hal/` — Linux, Zephyr and ESP-IDF HAL, TCP/file/OS adapters, sinks.
- `platform/` — board applications, the shared output owner and native harness.
- `tests/`, `tools/` — host tests and the clang-format/tidy/clangd checker.
- `docs/` — design contracts, development workflow and bench reports.

Zephyr module integration is in `zephyr/`; `west.yml` pins Zephyr 4.3.0 and the
sample adds this repository as an extra module.

## Contributing

Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for
prerequisites, the build/test/lint commands, the design constraints to
preserve, and the hardware-testing rules.

Parts of this project were developed with AI assistance (DeepSeek and GPT
Astra). Every generated change is reviewed, built and tested before it is
committed; see the disclosure in [CONTRIBUTING.md](CONTRIBUTING.md#ai-assisted-development).

## Scope

### Implemented

- Native WAV file playback and PCM/FLAC/MP3/Opus/Vorbis decoder adapters
- Zephyr kernel/HAL integration and an ESP-IDF v6 HAL/smoke target
- Zephyr and ESP-IDF Classic A2DP SBC sink bindings and a portable SBC
  ingress/decoder (hardware validation pending)
- Stereo I2S adapters (hardware validation in progress)
- A cleartext Sendspin player with configurable codec negotiation
- TCP/HTTP raw-PCM LMS client.

---

### Unfinished

- Bluetooth hardware validation, pairing/profile ergonomics, source arbitration
  and AAC/LDAC codecs
- Compressed-codec board integration, live FLAC/MP3 decode-thread wiring and MCU
  resource qualification
- ESP-IDF board-output integration and hardware I2S investigation
- PLL/ASRC synchronization
- Multi-room timing
- Release-qualification stress

Neither native_sim nor host tests establish physical DMA or multi-room timing
guarantees.
