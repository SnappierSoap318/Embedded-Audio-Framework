# Development workflow

Commit each completed, validated implementation stage locally. Use focused
messages explaining the resulting behavior. Do not push without a request.

The initial history was reconstructed from the existing working tree because
`.git` was empty. Milestone commits group the current implementation by dependency;
they are not recovered historical snapshots. They include the current formatting
and lint cleanup. Later work should be committed as it is completed.

## Clang tooling

Clangd uses `.clang-format` for editor formatting. `.clangd` points at `build-lint`:

```sh
cmake -S . -B build-lint -G Ninja -DCMAKE_C_COMPILER=clang \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-lint
python3 tools/check_code.py
ctest --test-dir build-lint --output-on-failure
```

The SBC decoder is vendored under `third_party/libsbc`, so no path is needed.
Set `EAF_LIBSBC_ROOT=/path/to/libsbc` only to build against an external checkout
instead. The checker runs
clang-format in verification mode, clang-tidy analyzer/bugprone checks and clangd
parsing on project translation units and included headers. It excludes external
vendor/generated sources. Clangd refactoring-tweak self-tests are disabled because
Clang 22 reports replacement-overlap failures unrelated to code diagnostics.

For Zephyr, configure with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, then run:

```sh
python3 tools/check_code.py build-zephyr-bt-lms
```

The checker writes an adapted compile database inside the build directory,
removing GCC-only optimization flags and translating `-fno-freestanding` to
Clang's `-fhosted`. It preserves defines and include paths; the normal Zephyr
build still uses its original compiler arguments. To edit Zephyr sources in
clangd, point `.clangd` at `build-zephyr-bt-lms/eaf-clang` locally after generating it.

Clang-tidy excludes the blanket Annex K replacement warning because these
platforms use bounded standard C buffer operations, not optional `_s` APIs.
Reserved POSIX feature macros and linker-wrapper identifiers are allowed by name.
Two local annotations document binary opcode bytes and intentional integer
rounding in the DSP reference test. Other configured warnings remain errors.

C sources, headers and fixture includes use clang-format. It does not format
Python, Markdown, Kconfig or CMake files. Those retain their native syntax.

For future work, choose a task/acceptance gate from `TASKS.md`, consult the audit,
and add an independent failing regression before correcting behavior. CTest's
`architecture` case guards portable source boundaries. Hardware results must be
recorded separately from simulator/host passes. See `docs/audit.md` for the
squeezelite-esp32 reference-test methodology.

## Architecture and backend checks

[Architecture v0.5](architecture-v0.5.md) defines current ownership/lifecycle
contracts. `arch_example` compiles and runs its finite-source example. The native
players link a CMake-selected ALSA/null adapter and have no feature preprocessor
branches. Check the fallback explicitly, even on machines with ALSA installed:

```sh
cmake -S . -B build-no-alsa -DEAF_ENABLE_ALSA=OFF -DBUILD_TESTING=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-no-alsa
ctest --test-dir build-no-alsa --output-on-failure
python3 tools/check_code.py build-no-alsa
```

`sink_lifecycle` injects partial init, START, write and cleanup failures;
`alsa_failures` injects write/DROP/drain failures and a deterministic drain deadline.
Zephyr smoke repeats write/START/DROP errors beyond the four-slot pool capacity,
then tests EOS drain failure and success. These mocks check software ownership;
ESP32 DMA timing, actual EOF tail and driver starvation recovery remain board gates.
Sink implementers must return checked errors from deinit and preserve a retry-safe
context. Update custom sink operation tables for the new `int` return signature.

## Scheduling HAL checks (T04)

Linux requires `sem_clockwait` and GNU pthread affinity attributes; CMake fails
clearly if the monotonic wait API is unavailable. `hal_os` tests coalescing wakes,
monotonic expiration with an injected EINTR, 5,000 contended wake/ack handshakes,
actual allowed-CPU pinning, and both FIFO role priorities with injected EPERM.
The denied-request test does not require realtime privileges. The native CLI uses
ordinary scheduling; callers opting into FIFO must handle EAF_IO on denial.
`hal_sem_take` now returns EAF_TIMEOUT on expiry rather than the former EAF_IO.

```sh
cmake --build build-p0-tsan --target test_hal_os test_lms_startup
ctest --test-dir build-p0-tsan -R '^(hal_os|lms_startup)$' --output-on-failure
```

Zephyr smoke verifies priorities 3 (audio) / 5 (decoder), repeated slot reuse,
binary wakes and monotonic timeout. Replace old CONFIG_EAF_THREAD_PRIORITY settings
with CONFIG_EAF_AUDIO_PRIORITY / CONFIG_EAF_DECODER_PRIORITY. Add `affinity.conf`
to the smoke's EXTRA_CONF_FILE list to exercise CPU-mask-enabled creation on CPU 0;
without it, the smoke verifies unsupported affinity requests leave no thread handle.
This native_sim configuration verifies API behavior, not multi-core ESP32 placement.

Before closing T04 on the board, record block period, worst wake-to-commit latency
and DSP compute time separately, under Wi-Fi and Bluetooth traffic. Exclude sink
acquisition waits from the DSP compute measurement. Check the architecture's 40%
compute budget and DMA queue margin with GPIO/cycle-counter evidence, document
priorities/CPU assignments, and retain observed overruns. No physical timing result
is implied by host sanitizer or simulated-kernel tests.

## WROOM LMS bench application

See [platform/esp32_lms](../platform/esp32_lms/README.md) for the actual ESP32
Wi-Fi/I2S build and local credential handling. For uploading firmware and
reading UART or wireless logs, follow [flash and inspect the ESP32](zephyr-setup.md#flash-and-inspect-the-esp32). The network controller and timed
null output also build under native_sim for host Clang diagnostics:

```sh
ZEPHYR_BASE="$PWD/build-deps/zephyr" cmake -S platform/esp32_lms -B build-lms-sim -G Ninja \
  -DBOARD=native_sim/native/64 -DZEPHYR_TOOLCHAIN_VARIANT=host \
  -DEXTRA_CONF_FILE=null.conf -DZEPHYR_MODULES="$PWD" \
  -DPython3_EXECUTABLE="$PWD/build-deps/venv/bin/python" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-lms-sim
python3 tools/check_code.py build-lms-sim
```

This simulator has no Wi-Fi driver and cannot connect; it checks compilation and
Clang diagnostics for the network controller, credentials adapter and output
worker. The Xtensa build checks the real ESP32 driver integration with strict GCC
warnings. `build-zephyr-bt-lms` supplies Clang coverage and runtime smoke for the
I2S HAL. Native `board_output` uses a timed null backend to exercise repeated track
lifecycle, mono expansion, gain/mute and pause without radio or amplifier hardware.

The current local dependency checkouts and Python/SDK tools live in ignored
`build-deps/` rather than `/tmp`. See [local Zephyr setup](zephyr-setup.md) for installation and environment commands.
Use a fresh build directory when moving Zephyr's source/toolchain paths; existing
CMake caches retain absolute paths. Wireless diagnostics were verified with
`build-wireless-sim` (Clang), `build-wroom-wireless` (I2S) and
`build-wireless-null` (silent backend). Native `board_log` checks bounded history,
truncation and concurrent reads/writes; optional Node-backed `web_page` checks
stalled-fetch recovery using the actual embedded page script.

## Sendspin application

The cleartext Sendspin client (captured MA 2.10.3 revision) and the shared board
output owner are documented in [the Sendspin plan](sendspin-plan.md) and
[the capture](bench/sendspin-capture-2026-09-13.md). Host modules live in
`apps/sendspin` (`sendspin_sync`, `sendspin_protocol`, `sendspin_ws`,
`sendspin_client`, `sendspin_player`) with `test_sendspin_*` regressions. The
protocol-agnostic reservoir/graph/sink/worker is `platform/esp32_output`; both
`platform/esp32_lms` and `platform/esp32_sendspin` build against it. Build the
board app like the LMS one (see its README) with `CONFIG_EAF_SENDPIN=y`, and the
host harness with:

```sh
cmake --build build-phase1 --target eaf_sendspin_probe
./build-phase1/eaf_sendspin_probe <ma-ip> 8927 15
```

`eaf_sendspin_probe` reaches `player@v1` activation and clock convergence
against a real MA. Physical WROOM playback (audible PCM, real-time intake) is the
open Phase 2 gate.

## ESP-IDF backend and smoke target

The ESP-IDF backend lives in `hal/esp_idf/` (`hal_os_esp_idf.c`,
`hal_tcp_esp_idf.c`, `sink_i2s_esp_idf.c` and the sink header) and mirrors the
Zephyr adapters: statically pooled FreeRTOS tasks with role-based priorities,
binary semaphores, `esp_timer` monotonic time, lwIP BSD sockets with
`TCP_NODELAY`, and a `driver/i2s_std.h` TX channel. The legacy `driver/i2s.h`
removed in IDF v6 is not used.

Prerequisites: ESP-IDF v6.x with the Xtensa toolchain. The local install is
v6.0.2; activate it before any command:

```sh
source ~/.espressif/tools/activate_idf_v6.0.2.sh
idf.py --version
```

`platform/esp_idf_sendspin/` is a standalone IDF project. It does not read the
repository root `CMakeLists.txt`; `components/eaf/CMakeLists.txt` is the manifest
that selects the portable sources (core, `hal/common`, the PCM and FLAC decoder
adapters and the vendored `dr_flac` implementation) plus the `hal/esp_idf`
adapters. `components/main/main.c` is a bounded smoke test: it checks the HAL
semaphore/thread/clock primitives, binds the I2S sink to the board pins, plays a
short 440 Hz tone and deinitializes cleanly. Board pins and smoke parameters come
from `components/main/Kconfig.projbuild`; scheduling and block size come from
`components/eaf/Kconfig`. The FreeRTOS tick defaults to 1000 Hz via
`sdkconfig.defaults`.

Build, flash and monitor for the classic `esp32` target (selected in
`sdkconfig.defaults`):

```sh
cd platform/esp_idf_sendspin
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The defaults match the WROVER + TAS5805M carrier (WS=25, BCLK=26, DOUT=22). Run
`idf.py menuconfig` to change pins or priorities. Note that FreeRTOS ranks a
larger numeric priority higher, so `CONFIG_EAF_AUDIO_PRIORITY` must exceed
`CONFIG_EAF_DECODER_PRIORITY`; this is the opposite ordering from the Zephyr
Kconfig symbols.

The I2S sink requests an APLL clock for `adjust_ppm` and falls back to the
default PLL if the APLL initialization fails, returning `EAF_UNSUPPORTED` for
rate adjustment in that case. The drain on EOS or pause is a bounded
DMA-residency settle, not a presentation timestamp, so the physical FIFO/amp tail
still needs a board measurement. Choosing an IDF target with different
capabilities or pin mapping requires a project under `platform/esp_idf_*` with
its own `sdkconfig.defaults` and board Kconfig.

Platform selection is by build system, never by `#ifdef CONFIG_*` in owned
translation units: Zephyr's `platform/esp32_*` CMake/Kconfig selects
`hal/zephyr` and `platform/esp32_output`; the ESP-IDF project's component
manifest selects `hal/esp_idf`; the native Linux build selects `hal/linux`.
