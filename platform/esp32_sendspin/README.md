# EAF Sendspin board player (WROOM)

ESP32-WROOM bench app that plays the cleartext Sendspin revision captured from
Music Assistant 2.10.3 (`ws://<ma-ip>:8927/sendspin`) through the shared board
output owner and the I2S sink. See
[docs/bench/sendspin-capture-2026-09-13.md](../../docs/bench/sendspin-capture-2026-09-13.md)
and [docs/sendspin-plan.md](../../docs/sendspin-plan.md).

## Build

Copy `credentials.example.h` to `credentials.local.h` and set the 2.4 GHz WPA2
SSID/passphrase. `CONFIG_EAF_BOARD_SERVER` (default `192.168.11.132`) and
`CONFIG_EAF_BOARD_PORT` (default `8927`) select the server. Credentials are
compiled into the firmware; treat build folders and images as private.

```sh
export PATH="$PWD/build-deps/venv/bin:$PATH"
export ZEPHYR_BASE="$PWD/build-deps/zephyr"
export ZEPHYR_SDK_INSTALL_DIR="$PWD/build-deps/zephyr-sdk-0.17.4"
cmake -S platform/esp32_sendspin -B build-wroom-sendspin -G Ninja \
  -DBOARD=esp32_devkitc/esp32/procpu -DZEPHYR_TOOLCHAIN_VARIANT=zephyr \
  -DPython3_EXECUTABLE="$PWD/build-deps/venv/bin/python" \
  "-DZEPHYR_MODULES=$PWD;$PWD/build-deps/hal-espressif;$PWD/build-deps/mbedtls" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-wroom-sendspin
```

Add `-DEXTRA_CONF_FILE=null.conf` for a silent connectivity build. For the
WROVER PSRAM profile add `-DEXTRA_CONF_FILE=psram.conf` and
`-DEXTRA_DTC_OVERLAY_FILE=psram.overlay`, which enables 8 MB SPI RAM and moves a
65536-frame (~1.5 s) reservoir to the external heap; the WROOM profile keeps the
4096-frame (~93 ms) reservoir in internal RAM.

The WROOM board config advertises mono PCM (`CONFIG_EAF_SOURCE_CHANNELS=1`,
`CONFIG_EAF_BYTES_PER_FRAME=2`) to halve the network bitrate; the PSRAM profile
advertises stereo. The board always expands the source to stereo for the sink.

## Behaviour

- Wi-Fi + DHCP, then `eaf_sendspin_client` connects to the configured server.
- `stream/start` configures the shared output owner and begins the portable
  producer (`eaf_sendspin_player`): PCM16 -> Q1.31 stereo, hard sync (drop late
  chunks by `compute_client_time`), mono expanded to stereo.
- 4096-frame reservoir (~93 ms stereo) is reported to the server as
  `buffer_capacity`; `required_lead_time_ms`/`min_buffer_ms` are derived from it.
- Binary Type-4 chunks are written to the reservoir; the audio worker feeds the
  DSP pipeline and I2S sink.

Select **EAF Sendspin WROOM** in Music Assistant and play a track. UART logs
report `chunks/written/dropped/underruns` every 5 seconds.

## Qualification

Builds for `native_sim` (null sink) and `esp32_devkitc/esp32/procpu` (I2S).
Physical audible playback, real-time intake and long-play stability are **not**
yet verified on hardware; those are the Phase 2 gate.
