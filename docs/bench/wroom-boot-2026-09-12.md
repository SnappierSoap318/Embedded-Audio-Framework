# First physical WROOM boot — 2026-09-12

Result: **PASS on the physical board**, with amplifiers disconnected.

ROM identification on `/dev/ttyUSB0` (1a86 USB serial bridge): ESP32-D0WD-V3,
revision 3.1, 40 MHz crystal, 4 MB flash. The user identifies the module as WROOM
without PSRAM; the carrier name/markings still need confirmation before GPIO wiring.
The firmware uses the upstream `esp32_devkitc/esp32/procpu` boot/UART template with
PSRAM disabled, simple boot at 0x1000, DIO flash at 40 MHz. It runs on PROCPU only.
Bluetooth, networking and I2S are disabled; no amplifier signals are configured.

## Build and memory evidence

Application and commands: [ESP32 boot test](../../platform/esp32_boot/README.md).
Zephyr v4.3.0 and its pinned Espressif HAL, Zephyr SDK 0.17.4 (GCC 12.2.0),
esptool 5.4.0. Official SDK archives were checked against release SHA-256 sums.
The installed ESP-IDF GCC 15.2.0 build was rejected due to incompatible libc/POSIX
thread types; no image from that failed build was flashed.

The SDK build linked with these region reports (regions can alias; do not sum them
as independent physical RAM):

| Region | Used | Region capacity |
| --- | ---: | ---: |
| FLASH | 148,948 bytes | 4,194,048 bytes |
| iram0_0_seg | 37,120 bytes | 224 KiB |
| dram0_0_seg | 78,568 bytes | 192 KiB |
| dram1_0_seg | 11 KiB | 96 KiB |

The reservoir uses 32,768 bytes and the physical build's diagnostic null sink uses
32,864 bytes. Kernel heap pool is configured to zero. The startup log nevertheless
reports a separate **115 KiB libc heap**; this is not a claim that the entire image
has no heap or that 115 KiB is available after adding Wi-Fi/Bluetooth. Those stacks,
codec scratch, DMA buffers and application state require a new memory/link-map check.

Flashed binary: 133,400 bytes, SHA-256
`285e2ec95141f683989f7e27ff8f494b7e0159fdf78751d99d9a16d4a5030596`.
Esptool reported `Hash of data verified`. A subsequent hardware reset was captured
at 115200 baud for 18 seconds.

## Physical serial evidence

```text
I (32) soc_init: ESP Simple boot
I (32) soc_init: chip revision: v3.1
I (36) flash_init: SPI Speed      : 40MHz
I (39) flash_init: SPI Mode       : DIO
I (43) flash_init: SPI Flash Size : 4MB
I (82) boot: libc heap size 115 kB.
*** Booting Zephyr OS build v4.3.0 ***
EAF WROOM boot: internal RAM, null output, no DAC pins driven
Reservoir=32768 bytes (4096 frames); sink=32864 bytes
EAF boot PASS (0): source=4096 frames, underruns=0
EAF alive: uptime=207 ms, test=PASS
EAF alive: uptime=5207 ms, test=PASS
EAF alive: uptime=10207 ms, test=PASS
EAF alive: uptime=15207 ms, test=PASS
```

The finite test covers a gated decoder worker, SPSC reservoir, volume node,
EOF/drain through timed null output and checked teardown. The application then
remains alive with a five-second heartbeat. It also passed native_sim and
clang-format/clang-tidy/clangd checks on 11 native-compiled EAF translation units;
the physical build used the real Xtensa compiler and EAF strict warnings.

Ignored local artifacts: `build-wroom/zephyr/{zephyr.bin,zephyr.elf,zephyr.map}`,
`build-wroom/{flash.log,boot.log,flash-backup-20260912.bin,flash-backup-20260912.sha256}`.
The full previous 4 MB flash was backed up before writing; it is not committed.

## Remaining gates

This establishes boot and a small internal-RAM pipeline, not physical audio,
full LMS/A2DP feasibility, SMP/radio coexistence, or scheduling margin under load.
Next identify the carrier and both MAX98357 breakouts, confirm A/B variants and
channel-selection circuitry, then choose pins and wire the I2S-only test together.
T04 timing, T05 final carrier/pin configuration and T06 physical stereo remain open.
