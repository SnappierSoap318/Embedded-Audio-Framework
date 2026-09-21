# EAF Sendspin board player (ESP32)

ESP32 bench app that plays the cleartext Sendspin revision captured from
Music Assistant 2.10.3 (`ws://<ma-ip>:8927/sendspin`) through the shared board
output owner and the I2S sink. It runs on the WROOM (Wi-Fi throughput limited
for stereo) and the WROVER with TAS5805M (the current audible target). See
[docs/bench/sendspin-capture-2026-09-13.md](../../docs/bench/sendspin-capture-2026-09-13.md)
and [docs/sendspin-plan.md](../../docs/sendspin-plan.md).

## Configuration

Copy `credentials.example.h` to `credentials.local.h` and set the 2.4 GHz WPA2
SSID/passphrase (ignored by git). Credentials are compiled into the firmware;
treat build folders and images as private.

The server address and player identity are Kconfig options:

| Option | Default | Meaning |
|---|---|---|
| `CONFIG_EAF_BOARD_SERVER` | `""` (required) | Music Assistant IPv4 address |
| `CONFIG_EAF_BOARD_PORT` | `8927` | Sendspin WebSocket port |
| `CONFIG_EAF_BOARD_NAME` | `EAF Sendspin` | Name shown in Music Assistant |
| `CONFIG_EAF_BOARD_CLIENT_ID` | `eaf-sendspin` | Stable player identifier |
| `CONFIG_EAF_SAMPLE_RATE` | `44100` | Source sample rate |
| `CONFIG_EAF_SOURCE_CHANNELS` | `2` | Source channels (WROOM advertises mono) |
| `CONFIG_EAF_BIT_DEPTH` | `16` | Source PCM precision (16, 24 or 32) |

Provide local values in an untracked `local.conf` next to this README:

```conf
CONFIG_EAF_BOARD_SERVER="192.168.1.10"
CONFIG_EAF_BOARD_NAME="Living Room"
```

## Build

```sh

```sh
export PATH="$PWD/build-deps/venv/bin:$PATH"
export ZEPHYR_BASE="$PWD/build-deps/zephyr"
export ZEPHYR_SDK_INSTALL_DIR="$PWD/build-deps/zephyr-sdk-0.17.4"
cmake -S platform/esp32_sendspin -B build-wroom-sendspin -G Ninja \
  -DBOARD=esp32_devkitc/esp32/procpu -DZEPHYR_TOOLCHAIN_VARIANT=zephyr \
  -DEXTRA_CONF_FILE=local.conf \
  -DPython3_EXECUTABLE="$PWD/build-deps/venv/bin/python" \
  "-DZEPHYR_MODULES=$PWD;$PWD/build-deps/hal-espressif;$PWD/build-deps/mbedtls" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-wroom-sendspin
```

Add `-DEXTRA_CONF_FILE=null.conf` for a silent connectivity build. For the
WROVER PSRAM profile add `-DEXTRA_CONF_FILE=psram.conf;local.conf` and
`-DEXTRA_DTC_OVERLAY_FILE=psram.overlay`, which enables 8 MB SPI RAM and moves a
65536-frame (~1.5 s) reservoir to the external heap; the WROOM profile keeps the
4096-frame (~93 ms) reservoir in internal RAM.

The WROOM board config advertises mono PCM (`CONFIG_EAF_SOURCE_CHANNELS=1`) to
halve the network bitrate; the PSRAM profile advertises stereo. The PCM bit
depth is `CONFIG_EAF_BIT_DEPTH` (16, 24 or 32); the player decodes any of the
three to Q1.31 and the TAS5805M is configured for 32-bit I2S words. The board
always expands the source to stereo for the sink.

## Behaviour

- Wi-Fi + DHCP, then `eaf_sendspin_client` connects to the configured server.
- `stream/start` configures the shared output owner and begins the portable
  producer (`eaf_sendspin_player`): PCM16 -> Q1.31 stereo, hard sync (drop late
  chunks by `compute_client_time`), mono expanded to stereo.
- 4096-frame reservoir (~93 ms stereo) is reported to the server as
  `buffer_capacity`; `required_lead_time_ms`/`min_buffer_ms` are derived from it.
- Binary Type-4 chunks are written to the reservoir; the audio worker feeds the
  DSP pipeline and I2S sink.

Select the player by its configured `CONFIG_EAF_BOARD_NAME` in Music Assistant
and play a track. UART logs
report `chunks/written/dropped/underruns` every 5 seconds.

## OTA updates (optional, MCUboot)

The default build uses the ESP32 simple boot, which has no OTA. To update over
Wi-Fi instead of UART, build the MCUboot profile and install the bootloader once.

> **Known issue:** on the WROVER/TAS5805M bench the MCUboot build accepts four
> I2S TX buffers and then times out waiting for a buffer to return, so playback
> stops with `I2S allocate TX block failed ... rc=-11`. Direct boot plays
> normally. Isolate the bootloader/build difference before relying on OTA. See
> [docs/bench/wrover-i2s-2026-09-20.md](../../docs/bench/wrover-i2s-2026-09-20.md).

One-time UART install (MCUboot at 0x1000, app in slot0 at 0x20000):

```sh
# Bootloader (once)
cmake -S build-deps/mcuboot/boot/zephyr -B build-mcuboot-esp32 -G Ninja \
  -DBOARD=esp32_devkitc/esp32/procpu -DZEPHYR_TOOLCHAIN_VARIANT=zephyr \
  -DPython3_EXECUTABLE="$PWD/build-deps/venv/bin/python" \
  "-DZEPHYR_MODULES=$PWD/build-deps/hal-espressif;$PWD/build-deps/mbedtls;$PWD/build-deps/mcuboot"
cmake --build build-mcuboot-esp32

# App with OTA
cmake -S platform/esp32_sendspin -B build-wrover-ota -G Ninja \
  -DBOARD=esp32_devkitc/esp32/procpu -DZEPHYR_TOOLCHAIN_VARIANT=zephyr \
  "-DEXTRA_CONF_FILE=psram.conf;ota.conf;local.conf" -DEXTRA_DTC_OVERLAY_FILE=psram.overlay \
  -DPython3_EXECUTABLE="$PWD/build-deps/venv/bin/python" \
  "-DZEPHYR_MODULES=$PWD;$PWD/build-deps/hal-espressif;$PWD/build-deps/mbedtls;$PWD/build-deps/mcuboot"
cmake --build build-wrover-ota

esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 write-flash \
  0x1000 build-mcuboot-esp32/zephyr/zephyr.bin \
  0x20000 build-wrover-ota/zephyr/zephyr.signed.bin
```

After that, flash over the network (the board responds on `/logs` at its IP):

```sh
curl --data-binary @build-wrover-ota/zephyr/zephyr.signed.bin http://<board-ip>/ota
```

The app writes `image-1`, requests a permanent upgrade and reboots; MCUboot swaps
and boots the new image. Partitions (4 MB layout): mcuboot `0x1000`, sys
`0x10000`, image-0 `0x20000`, image-1 `0x170000`, scratch `0x3e0000`.

## Qualification

Builds for `native_sim` (null sink) and `esp32_devkitc/esp32/procpu` (I2S).

Physical status (2026-09-20): the WROVER/TAS5805M direct-boot build plays stereo
44.1 kHz PCM audibly with server-driven volume and held zero underruns across a
multi-hour run. The MCUboot build still stalls I2S TX buffer completion; see the
note above. WROOM is throughput-limited for stereo. Remaining gates are release
qualification (long stress, recovery, synchronized playback), tracked in
`TASKS.md` (S03/S04/S06/T22/T23).
