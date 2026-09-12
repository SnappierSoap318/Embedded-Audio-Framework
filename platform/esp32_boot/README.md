# ESP32 WROOM boot test

This is the first physical-board bring-up step. It does not drive I2S/amplifier
pins or enable Wi-Fi/Bluetooth. A 4096-frame stereo reservoir occupies 32 KiB of
internal RAM; the diagnostic null sink adds about 32 KiB. A gated decoder worker
feeds 4096 frames through the volume node and timed null sink. The console reports
PASS/FAIL, source frames and underruns, then prints a heartbeat every five seconds.
This is not a memory budget for simultaneous LMS/A2DP or proof of audio deadlines.

Target: original ESP32, UART0 USB bridge, 4 MB flash, no PSRAM. The upstream
`esp32_devkitc/esp32/procpu` template selects a WROVER variant; the application
explicitly disables PSRAM in both Kconfig and devicetree for this WROOM boot test.
No carrier-specific amplifier pins are assigned. Confirm the carrier/breakout
markings before adding DAC wiring. The test uses Zephyr simple boot at 0x1000;
flashing replaces the existing firmware's boot image.

## Dependencies

- Zephyr v4.3.0: `3568e1b6d5cdd51a6b964a2a1d6d29200fea2056`.
- hal_espressif: `af6cfa2e3e7098b596062ab516b80a48a7ba7332` from that manifest.
- Zephyr SDK 0.17.4, `xtensa-espressif_esp32_zephyr-elf` toolchain.
- Zephyr Python build requirements and esptool (tested with 5.4.0).

The local ESP-IDF GCC 15.2.0/newlib combination is incompatible with this pinned
Zephyr configuration (conflicting POSIX thread types). Use the matching Zephyr SDK.
No RF blobs are needed for this radio-disabled boot image.

## Build

Replace the paths for your workspace. Put the Python environment's `bin` directory
on PATH so CMake can locate `esptool`. SDK archives should be checked against the
release's `sha256.sum` before extraction.

```sh
export PATH=/tmp/eaf-zephyr-venv/bin:$PATH
export ZEPHYR_BASE=/tmp/eaf-zephyr
export ZEPHYR_SDK_INSTALL_DIR=/tmp/zephyr-sdk-0.17.4
cmake -S platform/esp32_boot -B build-wroom -G Ninja \
  -DBOARD=esp32_devkitc/esp32/procpu -DZEPHYR_TOOLCHAIN_VARIANT=zephyr \
  -DPython3_EXECUTABLE=/tmp/eaf-zephyr-venv/bin/python \
  "-DZEPHYR_MODULES=$PWD;/tmp/eaf-hal-espressif" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-wroom
```

Before writing, read chip/flash identification and verify original ESP32 + 4 MB.
Save the existing flash if it is worth retaining. Device access may require dialout
permissions. Do not run the serial monitor and esptool simultaneously.

```sh
esptool --port /dev/ttyUSB0 chip-id
esptool --port /dev/ttyUSB0 flash-id
esptool --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  read-flash 0 0x400000 /tmp/wroom-backup.bin
esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 \
  write-flash 0x1000 build-wroom/zephyr/zephyr.bin
python -m serial.tools.miniterm /dev/ttyUSB0 115200
```

The console should show `EAF boot PASS (0): source=4096 frames, underruns=0`.
A successful write alone is insufficient: capture the boot output and heartbeat.
The same application can be built with `native_sim/native/64` and the host toolchain
for lifecycle regression; omit Espressif HAL from ZEPHYR_MODULES for that build.
