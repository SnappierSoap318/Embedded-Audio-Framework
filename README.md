# Embedded Audio Framework

Embedded C11 audio framework targeting Zephyr, with Linux as its test harness.
It provides a bounded SPSC audio reservoir, a static DSP pipeline, sink
adapters (I2S, ALSA, null), and network players built on a shared output owner:
a cleartext Sendspin player (Music Assistant) and a SlimProto/HTTP LMS client.

Licensed under the [GNU General Public License v3.0](LICENSE).

## Quick start (Linux host)

Requires CMake 3.20+, a C11 compiler, pthreads, libm, and glibc `sem_clockwait`.

```sh
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

### Board applications

| App | Board | README |
|---|---|---|
| Sendspin player | ESP32 (WROOM, WROVER + TAS5805M) | [platform/esp32_sendspin](platform/esp32_sendspin/README.md) |
| LMS player | ESP32-WROOM + MAX98357 | [platform/esp32_lms](platform/esp32_lms/README.md) |
| Boot/echo bring-up | ESP32 | [platform/esp32_boot](platform/esp32_boot/README.md) |

### Bench reports

- [Sendspin Phase 0 capture (MA 2.10.3)](docs/bench/sendspin-capture-2026-09-13.md)
- [WROVER I2S isolation and track-change fix](docs/bench/wrover-i2s-2026-09-20.md)
- [WROOM boot](docs/bench/wroom-boot-2026-09-12.md), [LMS registration](docs/bench/wroom-lms-2026-09-12.md),
  [ingress ring](docs/bench/wroom-lms-ingress-2026-09-13.md), [wireless diagnostics](docs/bench/wroom-wireless-diagnostics-2026-09-12.md),
  [MAX98357A wiring](docs/bench/wroom-max98357a-wiring.md)

## Status

Audible Sendspin playback is working on the ESP32-WROVER with a TAS5805M
amplifier: stereo 44.1 kHz PCM, server-driven volume/mute, zero observed
underruns across long runs, and manual track switching. The WROOM path is
Wi-Fi-throughput limited for stereo. The MCUboot/OTA build still stalls I2S TX
buffer completion at stream start, so direct boot is the working baseline; see
the [I2S bench report](docs/bench/wrover-i2s-2026-09-20.md).

`TASKS.md` is the authoritative remaining-work list. In brief: release
qualification and recovery, restored OTA, selectable audio formats, Bluetooth
A2DP, additional codecs, DSP controls, and synchronized multi-room playback.

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
- `apps/` — WAV decoder, player, LMS client, Bluetooth ingress/SBC, Sendspin.
- `hal/` — Linux and Zephyr HAL, TCP/file/OS adapters, sinks.
- `platform/` — board applications, the shared output owner and native harness.
- `tests/`, `tools/` — host tests and the clang-format/tidy/clangd checker.
- `docs/` — design contracts, development workflow and bench reports.

Zephyr module integration is in `zephyr/`; `west.yml` pins Zephyr 4.3.0 and the
sample adds this repository as an extra module.

## Scope

Implemented: native file playback, Zephyr kernel/HAL integration, a stereo I2S
adapter, a cleartext Sendspin player that is audible on the WROVER/TAS5805M
bench, and the TCP/HTTP raw-PCM LMS client. Unfinished: Bluetooth
pairing/profile negotiation and LC3 decoding, compressed file codecs, PLL/ASRC
synchronization, multi-room timing, and release-qualification stress. Neither
native_sim nor host tests establish physical DMA or multi-room timing
guarantees.
