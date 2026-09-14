# Hardware

Waveshare **ESP32-C6-Pico** (ESP32-C6-MINI-1, 4 MB flash, 512 KB SRAM, single-core RISC-V @ 160 MHz,
Wi-Fi 6 2.4 GHz only) with a Waveshare **Pico-DALI2** stacked on the Pico header.

## Pinout

| Signal | GPIO | Level / polarity | Notes |
|---|---|---|---|
| DALI TX | 14 | LOW = bus asserted (shorted), HIGH = bus released | `invert_tx = false` for the `espressif/dali` driver |
| DALI RX | 5 | HIGH = bus high (idle), LOW = bus low | `invert_rx = false` |
| RGB status LED | 8 | WS2812-type, driven via `espressif/led_strip` | Patterns in SPEC §11 |
| BOOT button | 9 | Active low | Strapping pin — read only after boot. Hold 5 s at runtime for factory reset |

All GPIO numbers are Kconfig options with these defaults and are overridable at runtime in the
configuration (`dali.tx_gpio`, `dali.rx_gpio`, `dali.invert_*`, `led.gpio`), so the firmware can be
retargeted to another board or transceiver without a rebuild.

TODO: photo of the assembled stack.
TODO: wiring diagram (ESP32-C6-Pico + Pico-DALI2 + external DALI PSU + control gear).

## Pico-DALI2 transceiver

From the Waveshare schematic:

- **Galvanic isolation** through two EL1018 optocouplers, one per direction. The MCU side and the
  bus side share no ground.
- **Bridge rectifier** on the bus input, so **bus polarity does not matter** — DA+/DA− can be
  swapped.
- **TX**: a 600 V MOSFET shorts the bus. Asserting GPIO14 LOW closes the switch and pulls the bus
  low; releasing it lets the bus return to ~16 V.
- **RX**: a 5.6 V zener plus a resistor divider sets the receive threshold, matching the DALI
  high/low bands.
- **1 A fuse** in the bus path.
- Optocoupler edges are **slow — tens of microseconds**. This is harmless: the RMT decoder rounds
  each measured interval to the nearest Te (416.7 µs), and the edge slew is an order of magnitude
  below half a Te.

## External DALI bus power supply

The Pico-DALI2 is a transceiver only: **it contains no bus power supply**. Without an external PSU
on the bus, TX shorts a dead bus and RX never sees a start bit, so no gear can ever answer.

Requirements per IEC 62386-101:

| Parameter | Value |
|---|---|
| Bus voltage | 16 V nominal (9.5–22.5 V allowed) |
| Bus current | ≤ 250 mA, current-limited by the PSU |
| Gear budget | ~2 mA per control gear, so the PSU current limit caps the bus population |

Wiring: the DALI PSU and every piece of control gear sit in parallel on the same two-wire bus, and
the Pico-DALI2 DA terminals connect to that same pair. Polarity is free (bridge rectifier). The bus
is SELV but is typically routed alongside mains wiring — use mains-rated cable and keep the two
separated per local regulation.

The firmware treats an unpowered bus as a **normal detectable condition**, not a fault: `bus_check`
(SPEC §7.4) reports `powered: false` when the RX idle level stays low, the bus object publishes it,
the LED blinks red, and everything else keeps running.

TODO: photo of a bench setup with a DIN-rail DALI PSU.
