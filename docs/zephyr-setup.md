# Local Zephyr setup

Keep dependencies in `build-deps/` under the repository root. Git ignores this
whole directory; it survives reboot and keeps external source and toolchains out
of project commits. Run the commands below from the repository root on Linux
x86-64. The current workspace already has these dependencies installed; do not
clone over existing directories.

## Pinned sources and Python

Install host prerequisites first: Git, CMake, Ninja, a C/C++ compiler, Python 3
with venv/pip support, curl, xz, and the device-tree compiler (`dtc`). Native
simulation also needs the host compiler. Then, on a fresh checkout:

```sh
mkdir -p build-deps
git clone https://github.com/zephyrproject-rtos/zephyr.git build-deps/zephyr
git -C build-deps/zephyr checkout --detach 3568e1b6d5cdd51a6b964a2a1d6d29200fea2056
git clone https://github.com/zephyrproject-rtos/hal_espressif.git build-deps/hal-espressif
git -C build-deps/hal-espressif checkout --detach af6cfa2e3e7098b596062ab516b80a48a7ba7332
git clone https://github.com/zephyrproject-rtos/mbedtls.git build-deps/mbedtls
git -C build-deps/mbedtls checkout --detach c5b06d89c9c498d8fc8659ce31f7e53137b6270f
python3 -m venv build-deps/venv
build-deps/venv/bin/python -m pip install -r build-deps/zephyr/scripts/requirements-base.txt
build-deps/venv/bin/python -m pip install -r build-deps/hal-espressif/zephyr/requirements.txt
```

These are the tested Zephyr v4.3.0 and associated module revisions. This minimal
CMake setup explicitly supplies modules; it does not require a west workspace.
Additional targets may require more modules from Zephyr's pinned `west.yml`.

## ESP32 compiler

Download SDK 0.17.4 and the original ESP32 toolchain, verify the release hashes,
and extract them locally:

```sh
(
  set -eu
  cd build-deps
  sdk_release=https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.4
  curl -fLO "$sdk_release/sha256.sum"
  curl -fLO "$sdk_release/zephyr-sdk-0.17.4_linux-x86_64_minimal.tar.xz"
  curl -fLO "$sdk_release/toolchain_linux-x86_64_xtensa-espressif_esp32_zephyr-elf.tar.xz"
  sha256sum --check --ignore-missing sha256.sum
  tar -xf zephyr-sdk-0.17.4_linux-x86_64_minimal.tar.xz
  tar -xf toolchain_linux-x86_64_xtensa-espressif_esp32_zephyr-elf.tar.xz -C zephyr-sdk-0.17.4
)
```

An explicit `ZEPHYR_SDK_INSTALL_DIR` avoids global SDK registration. These bench
targets use the toolchain directly; the bundled host-tools installer is not needed.
Do not substitute the ESP-IDF compiler: the tested IDF GCC/newlib combination has
incompatible POSIX declarations for this Zephyr configuration.

## Wi-Fi radio libraries

The radio-disabled boot test needs no blobs. LMS Wi-Fi needs the original ESP32
libraries listed in the pinned HAL manifest. Read the license at
`build-deps/hal-espressif/zephyr/blobs/license.txt`. The following fetches only
`lib/esp32/` entries, verifies every SHA-256, and reuses already verified files:

```sh
build-deps/venv/bin/python - <<'PY'
from pathlib import Path
import hashlib
import urllib.request
import yaml

hal = Path('build-deps/hal-espressif')
manifest = yaml.safe_load((hal / 'zephyr/module.yml').read_text())
for blob in manifest['blobs']:
    if not blob['path'].startswith('lib/esp32/'):
        continue
    target = hal / 'zephyr/blobs' / blob['path']
    data = target.read_bytes() if target.exists() else b''
    if hashlib.sha256(data).hexdigest() != blob['sha256']:
        with urllib.request.urlopen(blob['url'], timeout=60) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != blob['sha256']:
            raise RuntimeError(f"Checksum mismatch: {blob['path']}")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    print(f"Verified {blob['path']}")
PY
```

## Environment and builds

Set these in each new shell, from the repository root:

```sh
export PATH="$PWD/build-deps/venv/bin:$PATH"
export ZEPHYR_BASE="$PWD/build-deps/zephyr"
export ZEPHYR_SDK_INSTALL_DIR="$PWD/build-deps/zephyr-sdk-0.17.4"
export CCACHE_DIR="$PWD/build-deps/ccache"
```

Follow the [boot test](../platform/esp32_boot/README.md#build) or
[LMS player](../platform/esp32_lms/README.md#build-and-credentials) build commands.
For simulator and Clang checks, see [development](development.md#wroom-lms-bench-application).
Keep Wi-Fi credentials in the ignored local header described by the LMS guide.

Use a fresh build directory after moving dependencies: CMake caches contain
absolute paths. The current migrated builds are `build-wroom-wireless` (I2S),
`build-wireless-null` (silent ESP32) and `build-wireless-sim` (host diagnostics).
Older build directories can still contain `/tmp` paths and should be reconfigured
in fresh directories before reuse. No source, SDK, blob or virtual environment
belongs in Git.

## Flash and inspect the ESP32

Run from the repository root after setting the environment above. Connect the
board with a USB data cable. Close any serial monitor before flashing. These
commands target the original ESP32 WROOM with 4 MB flash and the bench app's
simple-boot image at `0x1000`; do not reuse that offset for other boot layouts.
Flashing replaces the installed firmware.

Select the serial port and build directory:

```sh
ls /dev/serial/by-id/
export EAF_SERIAL_PORT=/dev/ttyUSB0
export EAF_FLASH_BUILD=build-wroom-wireless
```

`build-wroom-wireless` is the current I2S build. If you followed the fresh LMS
build example, use `build-wroom-lms` instead. Use `build-wireless-null` for the
current silent connectivity image, or `build-wroom` for the UART-only boot test.
A `/dev/serial/by-id/...` path can replace `/dev/ttyUSB0` for stable identification.

Confirm the chip and flash size, then optionally save the existing 4 MB flash
before replacing it. The backup can contain credentials; keep it in ignored
local storage:

```sh
esptool --port "$EAF_SERIAL_PORT" chip-id
esptool --port "$EAF_SERIAL_PORT" flash-id
mkdir -p build-deps/backups
esptool --chip esp32 --port "$EAF_SERIAL_PORT" --baud 460800 \
  read-flash 0 0x400000 "build-deps/backups/wroom-$(date +%Y%m%d-%H%M%S).bin"
```

Build successfully before issuing the separate flash command:

```sh
cmake --build "$EAF_FLASH_BUILD"
esptool --chip esp32 --port "$EAF_SERIAL_PORT" --baud 460800 \
  write-flash 0x1000 "$EAF_FLASH_BUILD/zephyr/zephyr.bin"
python -m serial.tools.miniterm "$EAF_SERIAL_PORT" 115200
```

Exit miniterm with **Ctrl+]**. If the boot messages have already scrolled past,
press the board's **EN/reset** button with the monitor open. If esptool remains
at `Connecting...`, hold **BOOT**, tap **EN**, and release BOOT once it connects.
Retry at `--baud 115200` if transfers are unreliable. A permission-denied error
requires serial-device access for your user (check the device's group and your
OS's serial-port permissions); do not run competing monitors on the same port.

For the LMS image, expect `Wi-Fi IPv4: ...` and `LMS connected`. Open
`http://<board-ip>/` for wireless application logs, or `/logs` for plain text.
Wireless logs require Wi-Fi and the HTTP service to be running; use UART for
boot failures. Follow the [LMS bench checks](../platform/esp32_lms/README.md#board-verification-when-wiring-is-ready)
to test playback. A successful flash verifies transfer, not working audio.
