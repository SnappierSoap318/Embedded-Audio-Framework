# WROOM Wi-Fi / LMS bench player

This application connects an original ESP32 without PSRAM to a 2.4 GHz WPA2
network, obtains IPv4 through DHCP, and registers with LMS using the Wi-Fi MAC.
The default server is `192.168.11.132:3483`; its web UI remains on port 9000.
Bluetooth is disabled for this first network/audio qualification.

## Build and credentials

Follow [local Zephyr setup](../../docs/zephyr-setup.md) to install the pinned SDK,
Zephyr, modules, Python environment and verified ESP32 radio libraries.
Additionally check out the Zephyr manifest's mbedTLS revision
`c5b06d89c9c498d8fc8659ce31f7e53137b6270f` at `build-deps/mbedtls`. Keep dependencies under the ignored `build-deps/`
directory so a reboot does not remove the toolchain.
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
export PATH="$PWD/build-deps/venv/bin:$PATH"
export ZEPHYR_BASE="$PWD/build-deps/zephyr"
export ZEPHYR_SDK_INSTALL_DIR="$PWD/build-deps/zephyr-sdk-0.17.4"
cmake -S platform/esp32_lms -B build-wroom-lms -G Ninja \
  -DBOARD=esp32_devkitc/esp32/procpu -DZEPHYR_TOOLCHAIN_VARIANT=zephyr \
  -DPython3_EXECUTABLE="$PWD/build-deps/venv/bin/python" \
  "-DZEPHYR_MODULES=$PWD;$PWD/build-deps/hal-espressif;$PWD/build-deps/mbedtls" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-wroom-lms
```

For a **silent connectivity test**, use a separate `build-wroom-lms-null`
directory and add `-DEXTRA_CONF_FILE=null.conf`. This replaces audio output with
a timed null sink. The I2S image has passed physical Wi-Fi/DHCP and LMS registration;
audible output and the null image remain unqualified. See
[bench evidence](../../docs/bench/wroom-lms-2026-09-12.md).

## Board profiles

The application is one source tree with per-board configuration:

- **ESP32-WROOM (internal RAM, default).** `boards/esp32_devkitc_esp32_procpu.conf`
  disables PSRAM, uses a 4096-frame reservoir and a 16 KiB LMS ingress ring.
- **ESP32-WROVER-E / N16R8 (PSRAM).** Add `-DEXTRA_CONF_FILE=psram.conf
  -DEXTRA_DTC_OVERLAY_FILE=psram.overlay` to the configure command above and build
  in a separate directory (for example `build-wrover-lms`). This enables the 8 MB
  SPI RAM and allocates a 65536-frame (512 KiB, about 1.5 s) stereo reservoir from
  the external heap via `EAF_BOARD_USE_PSRAM`. The DevKitC board already selects
  the WROVER-E N4R8 SoC, so the 8 MB PSRAM node exists and the overlay re-enables it.

The profile is the output reservoir capacity and its placement; it is the buffer
that rides out network stalls. `CONFIG_EAF_BOARD_RESERVOIR_FRAMES` (power of two)
and `CONFIG_EAF_BOARD_USE_PSRAM` are in `platform/esp32_lms/Kconfig`; both build
profiles keep the same portable core and HAL.

## Board verification when wiring is ready

Follow [the wiring checks](../../docs/bench/wroom-max98357a-wiring.md) first.
The audible image uses GPIO26 BCLK, GPIO25 LRC and GPIO27 DIN, 32-bit stereo
slots. Both mono and stereo LMS PCM streams become stereo I2S. A fixed −10 dB
amplitude stage precedes LMS volume/mute for initial bench use.

Once the board is connected by USB and ready for replacement firmware, use the
boot app's identification/backup procedure, then:

```sh
esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 \
  write-flash 0x1000 build-wroom-lms/zephyr/zephyr.bin
python -m serial.tools.miniterm /dev/ttyUSB0 115200
```

Expect `Wi-Fi IPv4: ...`, the player MAC, and `LMS connected`. Select that player
in `http://192.168.11.132:9000/`; the previously identified board MAC is
`aa:bb:cc:dd:ee:ff`. The app advertises raw PCM support: compressed music requires
server-side conversion. Begin with a short 44.1/48 kHz PCM track and low LMS
volume. Verify left/right separately, mute, pause/resume, stop, and a sample-rate
change. Then test AP loss and server restart while retaining serial logs.

## Ownership and limits

The LMS/main thread is the sole producer and graph lifecycle owner. It creates a
gated audio worker before START; the worker processes 128-frame blocks at Zephyr
priority 3, above main's priority 5. The stereo reservoir holds 4096 Q31 frames
(32 KiB), with a preferred 3072-frame prebuffer. Gain updates use a bounded atomic mailbox;
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
stack headroom under network traffic. The wireless diagnostic I2S build reports DRAM0 160864 B
of 192 KiB, DRAM1 58968 B of 96 KiB, and IRAM 70144 B of 224 KiB. ESP32
region accounting is linker-specific; do not sum these as independent RAM pools.
The null backend is compiled consistently with a 128-frame limit in this app;
its DRAM0/DRAM1 use is 164480/54872 B. Both variants link without PSRAM. Capture
runtime allocation failures and stack high-water marks on the board.

Host `board_output` regression covers repeated mono streams, sample-rate changes,
volume/mute, pause/resume and cancellation. Zephyr smoke checks I2S pause drain
failure and resumed submission. Native simulation validates software only.

## Wireless application diagnostics

Open `http://<board DHCP address>/` (last observed `http://192.168.8.176/`).
`/logs` also returns plain text for curl or a collector. The page refreshes every
2 seconds and aborts stalled fetches after 4 seconds before retrying. It uses no
external web assets. This is a read-only, unauthenticated LAN bench endpoint,
not a firmware updater or remote shell; no Wi-Fi credentials are included.

A fixed 24 × 160-byte history retains recent application events, including events
before Wi-Fi connected. It is lost on reset and older events are overwritten.
This is not a complete ROM/panic/driver serial capture. UART remains available.
Log writes do not wait for a busy history lock; dropped entries are counted.
The HTTP worker runs at priority 7 with bounded socket waits, below audio and LMS.

Every 5 seconds, playback diagnostics report received PCM bytes, queued output
bytes, submitted source time, underruns and the output-failure flag. A growing
underrun count indicates the source is not feeding output continuously; a full
queue alone is normal backpressure. Time counts source frames submitted, not an
independently measured speaker clock. A reachable page with LMS connection errors
points to the server/route separately from the amplifier circuit.

The original 32 × 128-byte RX pool implied an approximately 1365-byte default TCP
window. The bench configuration now allocates 64 × 256-byte RX fragments and an
explicit 8192-byte receive window. Socket/context budgets also accommodate the
HTTP listener and a browser client alongside LMS and DHCP. This change needs
sustained physical playback validation; it is not proof that every stall is fixed.

Use standalone playback for qualification. Timestamped `strmu`/`strmp` commands
used by synchronized playback currently return EAF_UNSUPPORTED and terminate the
LMS session. Timed starts, clock correction and proper sync status remain pending.
The fixed −10 dB gain stage still reduces maximum LMS volume. Do not
mistake that deliberate attenuation for evidence of a faulty speaker circuit.

The R1 receive pump admits up to 32 transport steps or 1000 microseconds per batch,
checking control before HTTP on each step. It exits on no progress. A busy batch
yields one Zephyr tick; idle/backpressure waits 2 ms. This is bounded polling, not
a new socket-readiness HAL. Stream connect/start/pause callbacks can take longer
than that admission budget. Logs now include received HTTP bytes/s (headers included),
would-block/backpressure counts, budget yields and minimum queued PCM frames after
source playback begins. The first failure's numeric stage and opcode bytes survive
close; stage names are declared in `eaf_lms_stage_t`. HAL socket errors remain EAF
codes rather than OS errno values. No physical throughput result is implied.

R2 holds the output worker while filling that reservoir. SlimProto stream
thresholds (KiB of source PCM) and output thresholds (tenths of a second) are
converted to frames, rounded up, and combined with the 3072-frame preference.
Requests exceeding 4096 frames are clamped and logged. The chosen watermark also
controls recovery after an underrun. At 48 kHz the preferred reserve is 64 ms;
this is a bounded WROOM policy, not a guarantee against longer network stalls.
Short or empty EOF qualifies for release without reaching the watermark.
`cont` still gates PCM publication, and server-controlled start waits for `u`;
`STMl` reflects accepted reserve or short EOF. Pause and stop work while held.
No additional ingress ring or task is allocated. Existing underrun-tail handling
and completion/synchronization limitations remain for R3–R5. Physical long-play
and controlled network-gap tests are still required before closing R1/R2.

Worker diagnostics distinguish a full queue caused by pause from a stalled sink:
`flags` is a bitmask (active=1, released=2, pause requested=4, pause acknowledged=8,
worker exited=16). `calls` counts completed pipeline calls, including silence
while rebuffering. A released active worker normally reports 3; a paused one
reports 15. These independently sampled fields are diagnostic, not a synchronized
state snapshot. `HTTP`, `eof` and `gates` report HTTP-open, delivered input EOF,
and wait-cont/wait-start respectively. Pause callback requests are logged explicitly.
A full queue with unchanged calls and flags=3 warrants sink/worker investigation;
a full queue with flags=15 reflects a requested pause. Neither implies a proven
hardware or network cause by itself.

Periodic RX/output/worker telemetry is stored only in the bounded memory history;
it no longer prints synchronously to UART. Startup, stream lifecycle and errors
still appear on UART. For the logging comparison, play the same unsynchronized
track with the diagnostics browser tab closed for 20–30 seconds, then reopen it
and save the history. Compare receive rate, underrun growth and worker progress.
The 24-line history overwrites older entries; fetching it does not clear it.
This removes periodic UART writes, not formatting or HTTP polling overhead.
