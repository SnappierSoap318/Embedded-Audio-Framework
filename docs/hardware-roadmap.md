# Hardware bring-up sequence

The user is preparing an ESP32-WROOM with two MAX98357 amplifier modules and has
an ESP32-WROVER-IE/N16R8 board plus TAS5805MPWPR stereo amplifier on order. These
are planned targets, not validated board configurations. Confirm exact module
markings and carrier schematic before choosing devicetree target, memory settings
or pins. In particular, a memory suffix does not prove which ESP32 family is used.

## Bench 1: WROOM and two MAX98357 modules

Use an I2S-only board application first. Confirm whether the modules are MAX98357A
(standard I2S) or MAX98357B (left-justified); the current EAF sink selects standard
I2S. Both amplifiers receive the same stereo bus with individual channel selection.
Check the breakout's SD/MODE circuitry so one decodes left and the other right;
do not assume two identical breakout straps produce stereo. Use module schematics
and the manufacturer's [MAX98357 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX98357A-MAX98357B.pdf).

Share digital ground/BCLK/LRCLK/DIN as the circuit requires, and treat each speaker
output as a differential amplifier output—not a ground-referenced line output.
Confirm supply, load and pin selection from the actual modules. Start with mute
and low-level independent channel vectors. Verify 32-bit stereo framing with a
logic analyzer, then add DSP, then networking, then Bluetooth. No GPIO assignments
are committed until the board/pins are confirmed.

At 48 kHz stereo, Q31 consumes 384,000 bytes/second. A 200 ms reservoir alone needs
76,800 bytes; a 500 ms reservoir needs 192,000 bytes. Start with a measured internal
RAM budget on WROOM rather than copying the host's buffers or assuming PSRAM.
Record stacks, Wi-Fi/BT pools, packet queues, codec scratch and DMA slabs separately.

The pinned Zephyr 4.3 ESP32 HCI driver selects BTDM when both BT_CLASSIC and the
original ESP32 series are enabled (`drivers/bluetooth/hci/hci_esp32.c`). That is
source-level evidence of a supported controller path, not evidence this application
has paired or streamed. Real SDP/pairing, clock/memory budget and coexistence remain
T05–T09 in [TASKS.md](../TASKS.md).

## Bench 2: WROVER-IE and TAS5805M

First identify the actual N16R8 memory population and carrier pin map, then repeat
the I2S-only tests. Place the large reservoir in PSRAM only after verifying memory
configuration and keep DMA-owned buffers in internal DMA-capable RAM.

TAS5805MPWPR identifies an IC/package, not a complete board initialization profile.
The TAS5805M requires board-specific power/reset and I2C programming as well as
I2S. Clock availability during amplifier setup must be reconciled with EAF's
current deferred I2S START. Use the [TI TAS5805M datasheet](https://www.ti.com/lit/gpn/tas5805m)
for power/clock sequencing and validated register settings; do not copy a
LouderESP32 configuration without checking the actual carrier circuit.

Keep amplifier control in HAL and the selected profile/pins in platform code.
Test mute, startup ramps, fault reporting, thermal/load behavior and recovery
before high-power listening. Memory capacity and amplifier choice alone do not
prove sub-millisecond synchronization or 2.1 physical output support.

### Captured carrier pin map (2026-09-20)

Schematic "ESP32 Mainboard": ESP32-WROVER-E (U1) + TAS5805MPWP (U2).

| Signal | ESP32 pin | Notes |
| --- | --- | --- |
| `MCU_TX` / `MCU_RX` | GPIO1 / GPIO3 | UART0, 100 Ω series |
| `GPIO0` | GPIO0 | boot strap, exposed |
| `EN/CHIP_PU` | EN | `MCU_RST`; no RTS auto-reset wired |
| `I2C_DATA_SOC` / `I2C_CLK_SOC` | GPIO21 / GPIO27 | TAS5805M SDA / SCL |
| `I2S_WS` / `I2S_CLK` / `I2S_DATA_OUT` | GPIO25 / GPIO26 / GPIO22 | TAS5805M LRCLK / BCLK / DIN |
| `AMP_PWDN` | GPIO33 | TAS5805M PDN; must be driven high to enable |
| `AMP_FAULT` | GPIO34 | TAS5805M FAULT, input (active low) |
| `RGB_DATA` | GPIO13 | status LED |
| `SENSOR_VP` / `SENSOR_VN` | GPIO36 / GPIO39 | unused on this carrier |

TAS5805M outputs (OUT_A/B) feed 10 µH + 470 nF LC filters to the speakers;
`AMP_PWDN` and I2C configuration are required before any audio. Flash is 16 MB,
PSRAM 8 MB (4 MB addressable mapped). Bootloader entry is manual (hold GPIO0 low
across EN).
