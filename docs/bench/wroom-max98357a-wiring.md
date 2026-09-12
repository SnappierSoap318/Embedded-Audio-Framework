# WROOM + two MAX98357A modules: wiring preparation

The supplied photos show a 30-pin USB-C ESP32 carrier with D25/D26/D27 labels,
and a purple mono MAX98357A breakout with LRC, BCLK, DIN, GAIN, SD, GND and VIN.
They identify the accessible labels and A variant, but do not establish the
carrier's VIN/USB power routing or a complete amplifier schematic.
The board currently runs the UART-only boot test; it does not generate I2S.

## Shared signal bus

Disconnect USB and amplifier power before wiring. Use the printed GPIO labels,
not header-position numbers; keep connections short.

| ESP32 label | Both amplifier modules | Purpose |
| --- | --- | --- |
| D26 / GPIO26 | BCLK | Bit clock |
| D25 / GPIO25 | LRC | Left/right word clock |
| D27 / GPIO27 | DIN | Interleaved stereo data |
| GND | GND | Common reference |

These are the proposed firmware pin assignments. Zephyr's pinned ESP32 pinmux
headers provide I2S0 output routes for all three. The MAX98357A accepts standard
I2S and does not require MCLK. Neither module needs I2C configuration.

## Power and speakers — confirm before powering

Prefer a regulated external 5 V amplifier supply with sufficient current for both
speakers. The ESP32 can remain USB-powered: join supply negative, ESP32 GND and
both amplifier GND pins, and connect supply positive only to amplifier VIN pins.
Do not connect external 5 V to the USB-powered carrier VIN without confirming its
power circuit. Do not power the amplifiers from the carrier's 3V3 regulator.
The user can obtain a regulated 5 V / 2 A supply and has a multimeter and
10 kΩ/20 kΩ resistors. Speakers are reported as Sony XS-FB162E, 40 W, 8 Ω.
Sony's model specification confirms 40 W rated power; nominal impedance should
still be checked on each physical speaker label (do not substitute specifications
for a similarly named Sony model). Either a 4 Ω or 8 Ω speaker is suitable for this
low-power MAX98357A test, one per amplifier. The speaker's 40 W power handling is
not a minimum amplifier requirement. At 5 V, the MAX98357A provides approximately
1.4 W into 8 Ω or 2.5 W into 4 Ω at 1% THD, per the referenced breakout guide.
A 5 V / 2 A supply is sufficient for the two-module low-level bench test.

Each speaker connects only across its own amplifier's marked + and - outputs.
Neither speaker terminal is ground; never join the speaker negative terminals.
Initially connect each SD to GND (shutdown) and leave GAIN unconnected. The expected
floating-GAIN setting is 9 dB; use a low-amplitude firmware test when enabling it.

## Separate left/right selection

Both boards receive the same I2S bus. Leaving both SD pins floating does not
establish stereo: boards with the usual 1 MΩ VIN pull-up typically mix L/R at 5 V.
The photo shows a resistor marked 105 (1 MΩ), but its routing is not fully verified.

For the later enabled test, remove each SD-to-GND shutdown jumper:

- Left: SD directly to ESP32 3V3.
- Right: 3V3 → 20 kΩ → SD → 10 kΩ → GND. This divider targets roughly 1.1 V;
  including a typical internal 100 kΩ pull-down and a 1 MΩ pull-up to 5 V gives
  approximately 1.06 V. Two 10 kΩ resistors in series can form the 20 kΩ leg.

Verify the right SD voltage with a multimeter before the listening test. Target
about 1.0–1.2 V, away from channel-selection thresholds. The left SD should be
about 3.3 V. A different breakout resistor network may require adjustment; do not
infer the selected channel solely from the resistor marking. The signal wiring
can now be prepared using the supply and resistor arrangement above.

## First powered checks (current silent boot firmware)

1. With USB and amplifier supply disconnected, finish the shared signal/ground
   wiring and connect external +5 V to both amplifier VIN pins. Keep the ESP32
   powered via USB only; do not join external +5 V to carrier VIN or 3V3.
2. Remove the temporary direct SD-to-GND shutdown jumpers and wire left SD to
   3V3 and the right SD resistor divider. Leave GAIN open on both modules.
3. Leave the speakers disconnected for these voltage checks. Power the ESP32 and
   amplifiers, and measure relative to common GND: both VIN approximately 5 V,
   left SD approximately 3.3 V, right SD approximately 1.0–1.2 V. Report readings
   before moving to audio firmware; the current boot image generates no I2S.
4. Disconnect both power sources before attaching each speaker to its own +/−
   amplifier outputs. Neither output terminal connects to common GND or the other
   amplifier. Begin listening only with the planned low-amplitude channel test.

References:
- [MAX98357A/B datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX98357A-MAX98357B.pdf): I2S, supplies and SD_MODE selection.
- [Adafruit mono breakout pinout](https://learn.adafruit.com/adafruit-max98357-i2s-class-d-mono-amp/pinouts): analogous breakout pull-up, gain settings, 3.3 V logic with 5 V supply and bridge-tied speaker outputs. The pictured clone is not assumed to have an identical schematic.

Speaker reference: [Sony XS-FB162E specifications](https://www.sony.co.in/electronics/car-speakers/xs-fb162e/specifications).
