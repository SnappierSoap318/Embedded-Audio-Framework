# WROOM Wi-Fi / LMS bench player

This application connects an original ESP32 without PSRAM to a 2.4 GHz WPA2
network, obtains IPv4 through DHCP, and registers with LMS using the Wi-Fi MAC.
The default server is `192.168.11.132:3483`; its web UI remains on port 9000.
Bluetooth is disabled for this first network/audio qualification.

## Build and credentials

Use the SDK and Zephyr/HAL revisions in [the boot app](../esp32_boot/README.md).
Additionally check out the Zephyr manifest's mbedTLS revision
`c5b06d89c9c498d8fc8659ce31f7e53137b6270f` at `/tmp/eaf-mbedtls`.
Install the ESP32 radio libraries declared by the pinned HAL's
`zephyr/module.yml` using Zephyr's `west blobs fetch hal_espressif` in a configured
west workspace (or fetch the manifest URLs and verify their declared SHA-256
hashes). Observe the supplied Espressif binary licenses. Dependencies and build
output must stay outside version control.

```sh
cp platform/esp32_lms/credentials.example.h platform/esp32_lms/credentials.local.h
```

Edit the two empty strings in `credentials.local.h` locally. This ignored file
accepts a 1–32 byte SSID and an 8–63 byte WPA2 passphrase as C string literals;
escape quotes/backslashes if needed. Do not paste credentials into Git or logs.
Open networks, WPA3-only networks and enterprise authentication are not supported
by this bench app. Without credentials the image builds but exits with a UART
setup message. Credentials are compiled into the firmware, so treat the build
folder and firmware as private too. Reconfigure/rebuild after editing credentials.

```sh
export PATH=/tmp/eaf-zephyr-venv/bin:$PATH
export ZEPHYR_BASE=/tmp/eaf-zephyr
export ZEPHYR_SDK_INSTALL_DIR=/tmp/zephyr-sdk-0.17.4
cmake -S platform/esp32_lms -B build-wroom-lms -G Ninja \
  -DBOARD=esp32_devkitc/esp32/procpu -DZEPHYR_TOOLCHAIN_VARIANT=zephyr \
  -DPython3_EXECUTABLE=/tmp/eaf-zephyr-venv/bin/python \
  "-DZEPHYR_MODULES=$PWD;/tmp/eaf-hal-espressif;/tmp/eaf-mbedtls" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-wroom-lms
```

For a **silent connectivity test**, use a separate `build-wroom-lms-null`
directory and add `-DEXTRA_CONF_FILE=null.conf`. This replaces audio output with
a timed null sink. The I2S image has passed physical Wi-Fi/DHCP and LMS registration;
audible output and the null image remain unqualified. See
[bench evidence](../../docs/bench/wroom-lms-2026-09-12.md).

## Board verification when wiring is ready

Follow [the wiring checks](../../docs/bench/wroom-max98357a-wiring.md) first.
The audible image uses GPIO26 BCLK, GPIO25 LRC and GPIO27 DIN, 32-bit stereo
slots. Both mono and stereo LMS PCM streams become stereo I2S. A fixed 1/8
amplitude stage (about −18 dB) precedes LMS volume/mute for initial bench use.

Once the board is connected by USB and ready for replacement firmware, use the
boot app's identification/backup procedure, then:

```sh
esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 \
  write-flash 0x1000 build-wroom-lms/zephyr/zephyr.bin
python -m serial.tools.miniterm /dev/ttyUSB0 115200
```

Expect `Wi-Fi IPv4: ...`, the player MAC, and `LMS connected`. Select that player
in `http://192.168.11.132:9000/`; the previously identified board MAC is
`70:4b:ca:6d:c2:2c`. The app advertises raw PCM support: compressed music requires
server-side conversion. Begin with a short 44.1/48 kHz PCM track and low LMS
volume. Verify left/right separately, mute, pause/resume, stop, and a sample-rate
change. Then test AP loss and server restart while retaining serial logs.

## Ownership and limits

The LMS/main thread is the sole producer and graph lifecycle owner. It creates a
gated audio worker before START; the worker processes 128-frame blocks at Zephyr
priority 3, above main's priority 5. The stereo reservoir holds 4096 Q31 frames
(32 KiB), with a 1024-frame prebuffer. Gain updates use a bounded atomic mailbox;
pause is acknowledged by the audio worker and drains queued I2S blocks without
resetting the reservoir. Failed output cleanup halts reconnect to preserve owned
resources; inspect UART and reset after an output fault.

Wi-Fi association/DHCP waits up to 30 seconds and reconnect attempts are spaced
five seconds apart. LMS elapsed time counts submitted source frames, not measured
speaker presentation time. Full recovery under starvation, gapless transitions,
clock accuracy, physical DMA drain, coexistence, and long-running reconnect tests
remain pending. This application does not close T04, T06, T07 or T09 hardware gates.

EAF audio storage and worker pools are static. The Zephyr/Espressif network stack
uses a separately configured 64 KiB heap: this is an explicit platform exception
to the audio path's no-allocation contract. Link-time fit does not prove heap or
stack headroom under network traffic. The initial I2S build reports DRAM0 151496 B
of 192 KiB, DRAM1 40216 B of 96 KiB, and IRAM 70144 B of 224 KiB; ESP32 region
accounting is linker-specific, so do not add these as independent free RAM pools.
The null sink retains diagnostic sample buffers, so its DRAM0 use is higher
(183784 B of 192 KiB); it is not a smaller-memory alternative. Capture runtime
allocation failures and stack high-water marks on the board.

Host `board_output` regression covers repeated mono streams, sample-rate changes,
volume/mute, pause/resume and cancellation. Zephyr smoke checks I2S pause drain
failure and resumed submission. Native simulation validates software only.
