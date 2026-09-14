# esp_dali_gw

A DALI ⇄ MQTT gateway and DALI toolkit firmware for the Waveshare **ESP32-C6-Pico** with a
**Pico-DALI2** transceiver. It bridges one DALI bus (IEC 62386) to MQTT — for Calaos, Home Assistant
or any broker — and embeds a web UI that scans the bus, commissions short addresses, reads and
writes control-gear configuration and sends raw frames, with no cloud, no CDN and no broker
required. Licensed under Apache-2.0.

## Warning: the bus needs an external power supply

**The Pico-DALI2 has no DALI bus power supply.** It is a transceiver only. An external DALI PSU
(**16 V, ≤ 250 mA**) must be present on the bus or nothing will ever answer.

An unpowered bus is a **normal, detectable condition**, not a fault: the firmware reports
`powered: false` on `<base>/bus` and in the UI, blinks the status LED red, and keeps running. See
[docs/HARDWARE.md](docs/HARDWARE.md) for wiring the PSU.

## Wiring

| Signal | GPIO | Notes |
|---|---|---|
| DALI TX | **14** | LOW = bus asserted (shorted), HIGH = bus released. No inversion (`invert_tx = false`). |
| DALI RX | **5** | HIGH = bus idle, LOW = bus low. No inversion (`invert_rx = false`). |
| RGB status LED | **8** | On-board WS2812-type LED. |
| BOOT button | **9** | Factory reset: hold 5 s at runtime. Strapping pin, read only after boot. |
| Flash | — | 4 MB, dual OTA slots. |

All GPIO numbers are Kconfig options with these defaults and are also overridable at runtime in the
configuration, so the firmware can drive another board or a different transceiver.

## Flash a board

No toolchain needed: open **<https://calaos.github.io/esp_dali_gw/>** in Chrome or Edge, plug the
board in over USB-C and press the button. Flashing erases the device; an already-provisioned
gateway is better updated with the OTA upload in its own UI. See
[docs/RELEASING.md](docs/RELEASING.md).

## Build and flash

The [devcontainer](.devcontainer/) is the **only supported toolchain**; CI runs the byte-identical
image. Open the repo in VS Code / Claude Code (or `devcontainer up`), then:

```bash
cd web && npm ci && npm run build      # must run BEFORE idf.py build
cd ..
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash monitor   # the board enumerates as USB-JTAG/serial
```

The web UI is embedded into the firmware image, so `npm run build` has to come first — CMake fails
with a hint if `web/dist/` is missing.

Without a full devcontainer, any command can be run in the same image directly:

```bash
tools/docker-run.sh idf.py build
```

On macOS and Windows, Docker Desktop cannot pass a USB serial port into a container: build in the
container and flash from the host with `tools/flash.sh`.

## First boot

With no Wi-Fi credentials stored, the device starts an access point:

- SSID `ESP-DALI-GW-<id>`, WPA2 password `dali12345` (`<id>` = last 3 bytes of the MAC, lowercase hex)
- Browse to `http://192.168.4.1/` — any URL redirects there (captive portal) — and run the setup wizard.

After provisioning, the device joins your network and is reachable at
`http://esp-dali-gw-<id>.local/`. The full UI, bus tools included, also works in AP mode for on-site
diagnostics with no network at all.

## Status LED

| State | Pattern |
|---|---|
| AP provisioning | Blue, slow breathing |
| STA connecting | Yellow blink |
| Connected, MQTT down | Green, short double blink every 3 s |
| Connected, MQTT up | Green, very dim steady (off if the LED is disabled) |
| Bus unpowered / bus error | Red blink |
| Bus activity | Brief white flicker (≤ 20 ms) |
| Factory-reset hold | Red fast blink |
| OTA in progress | Purple breathing |

## Documentation

- [docs/SPEC.md](docs/SPEC.md) — the specification, source of truth for behaviour
- [docs/HARDWARE.md](docs/HARDWARE.md) — pinout, transceiver, bus PSU wiring
- [docs/MQTT_API.md](docs/MQTT_API.md) — topics and payloads
- [docs/HTTP_API.md](docs/HTTP_API.md) — REST routes and SSE stream
- [docs/RELEASING.md](docs/RELEASING.md) — cutting a release, artefacts, the browser flasher

## License

Apache-2.0 — see [LICENSE](LICENSE). Copyright 2026 Calaos.
