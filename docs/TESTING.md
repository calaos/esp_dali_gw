# On-device verification checklist

**Nothing in this firmware has run on hardware.** It was developed without a board, without a DALI
bus and without an MQTT broker: every milestone is compile-clean, host-tested where the code is pure
logic, and exercised against stub servers on the web side, but no gear has ever answered a frame.

This file lists what that leaves unproven, in the order worth doing it. Items are grouped by what
they need, so a session with a board but no bus can still clear a useful chunk.

## What each stage needs

| Stage | Needs |
|---|---|
| A. Board only | ESP32-C6-Pico over USB |
| B. Board + network | a Wi-Fi network and a phone or laptop |
| C. Board + broker | an MQTT broker reachable from the device |
| D. Board + powered bus | Pico-DALI2, an external DALI PSU (16 V, ≤ 250 mA) and at least one control gear |
| E. Full bench | several gears, at least one DALI-2 and ideally one DT8, plus a factory-fresh unaddressed one |

---

## Iterating without losing the configuration

`write-flash 0x0 <merged>.bin` covers the whole flash, **including NVS** — every re-flash wipes the
Wi-Fi credentials and starts provisioning again. While iterating, write only the app partition:

```bash
esptool --port /dev/ttyACM0 --baud 921600 write-flash 0x20000 build-<target>/esp_dali_gw.bin
```

Use the merged image for a first install, a recovery, or when the partition table changes.

## A. Board only

- [ ] It boots, and the banner prints a version from `git describe`, not `0.0.0-nogit`.
- [ ] The status LED shows **yellow blink** (STA connecting) or **blue breathing** (AP provisioning)
      — this is the first proof GPIO8 and the WS2812 timing are right.
- [ ] LED brightness from config takes effect without a reboot.
- [ ] Holding **BOOT for 5 s** blinks red fast and then erases the config and reboots into
      provisioning. Check a short press does nothing.
- [ ] `/api/info` answers, and `build.target` reports the right chip revision.
- [ ] With no bus wired, `bus_check` reports `powered: false` and the LED goes to **red blink**
      rather than the firmware treating it as a fault.
- [ ] Free heap after boot is sane and does not fall over the first hour.

## B. Board + network

- [ ] Provisioning from a phone: the AP `ESP-DALI-GW-<id>` appears, WPA2 with `dali12345` works,
      and the **captive portal sheet opens by itself** on iOS, Android and Windows. This is the
      single least-verifiable thing in the project — the DNS responder and the probe-URL list have
      never met a real client stack.
- [ ] The setup wizard scans, lists real networks with sane RSSI, and saves.
- [ ] After "save & reboot" the device joins and is reachable at `http://esp-dali-gw-<id>.local/`.
      mDNS resolution from macOS, Linux (avahi) and Windows all behave differently — check each you
      care about.
- [ ] **Fallback**: power the router off mid-session. Within `fallback_ap_timeout_s` the recovery
      AP comes up while STA keeps retrying; the backoff reaches its 30 s cap and never gives up.
- [ ] Power the router back on: STA reconnects and the AP is torn down after the 30 s grace.
- [ ] `/api/wifi/scan` works **while the AP is up** (APSTA) and comes back inside 5 s.
- [ ] Static IP: address, gateway and DNS are actually used.
- [ ] SNTP sets the clock; `last_scan` stops rendering as "never".
- [ ] SSE: the dashboard updates live; a fourth subscriber gets 503 and the banner says so;
      killing Wi-Fi mid-stream trips the 45 s watchdog and reconnects.
- [ ] OTA from the Settings page, including the progress bar, then reboot into the new image.
- [ ] **Rollback**: flash an image that crashes early and confirm the bootloader reverts to the
      previous slot. This is the safety net for every later OTA and is worth deliberately breaking
      an image to prove.
- [ ] HTTP Basic auth on, then off, and that the captive-portal probes still redirect while it is on.

## C. Board + broker

- [ ] Connect, and `<base>/status` is retained with `state: online`.
- [ ] **Pull power** (do not reboot cleanly): the LWT publishes `offline`.
- [ ] Retained `bus`, `gears` and `gear/<addr>/state` survive a broker reconnect.
- [ ] Changing `mqtt.*` from Settings restarts the client in place without a reboot and without
      leaking a subscription.
- [ ] `mosquitto_pub` a bare `128`, `ON` and `OFF` to `<base>/gear/3/set` — the bare forms are what
      a home-automation system actually sends.
- [ ] A `set_config` document larger than the 2 KB input buffer arrives reassembled.
- [ ] The log rate limiter under a real burst: scan an unpowered bus and confirm the first failure
      is published immediately and the rest are throttled.
- [ ] Home Assistant discovery: entities appear, brightness works, a DT8 gear exposes colour
      temperature, and **a gear that disappears after a scan removes its entity** rather than
      leaving a ghost.
- [ ] `tools/mqtt_cli.py` against the real firmware.

## D. Board + powered bus

Wire the PSU first — the transceiver has none. See `docs/HARDWARE.md`.

- [ ] `bus_check` flips to `powered: true`.
- [ ] **A single `set_level` on a known address changes a light.** Everything else depends on this
      one frame being right: GPIO14 polarity, the 416.67 µs half-period, and `invert_tx = false`.
- [ ] `QUERY ACTUAL LEVEL` returns a plausible byte — proves the RX path and the backward-frame
      window.
- [ ] A scan of 64 addresses finds exactly the gears present and takes roughly 4 s.
- [ ] Timing: confirm the 20 ms inter-frame gap is respected and that no reply is missed at the
      edges of the 25 ms window.
- [ ] `set_level` during a running scan is served between two scan steps rather than after it —
      this is the whole point of the queue design.
- [ ] Cancel a scan mid-run.
- [ ] Raw console: send a known frame and see the expected reply.
- [ ] The LED flickers white on bus activity.

## E. Full bench

- [ ] **Deep scan** reads config and memory bank 0. Check GTIN and serial against the label on the
      gear — the byte order and offsets in bank 0 are the likeliest thing to be wrong. The serial is
      read as 8 bytes from 0x0B (edition 2); an edition 1 gear may lay it out differently.
- [ ] `identity.bank0_version` is read from location 0x01, which edition 2 calls the memory bank 0
      version and edition 1 called reserved. An old gear may answer 0 or 0xFF; confirm against a
      real fitting before trusting the field.
- [ ] `configure` writes min, max, power-on, system-failure, fade time, fade rate; every parameter
      reads back `ok`. Deliberately write an out-of-range value and confirm it reports `mismatch`
      rather than silently passing.
- [ ] Scenes: program one, read it back, clear it, and confirm it reads as **not programmed**
      rather than as level 0.
- [ ] Groups: add, remove, and verify against `QUERY GROUPS`.
- [ ] **Commissioning `unaddressed`** on a factory-fresh gear. Then `all` on a small bus.
      Time it — the binary search is minutes on a full bus, and the progress events are what makes
      that bearable.
- [ ] **Cancel commissioning mid-search** and confirm ordinary commands still work afterwards. If
      `TERMINATE` did not go out, every gear ignores commands for 15 minutes — this is the single
      most consequential failure mode in the toolkit.
- [ ] Re-address a gear, and confirm a collision is refused.
- [ ] `identify` on a DALI-2 gear (one frame) and on a DALI-1 gear (the blink fallback).
- [ ] DT8: colour temperature within the gear's reported mirek bounds, and RGB on a colour gear.
      Confirm a DT6 gear refuses colour rather than doing something unpredictable.
- [ ] `poll_all` at the configured interval publishes only on change.
- [ ] Mixed bus: DALI-1 and DALI-2 gear together, since several fallbacks key off the version byte.

## M5, listen mode

- [ ] Frames the gateway sends are not reported back as observed traffic.
- [ ] A transaction is not measurably slower with listening on than with it off — compare a
      64-address scan both ways.
- [ ] A DALI-2 push-button press produces an event.
- [ ] The monitor in the raw console shows frames from another master or a DALI-2 input device.
- [ ] `DALI_LISTEN_IDLE_US` (1400 us) against real receiver pulse stretching: it must stay above a
      2 Te run and below the 7 Te backward-frame delay. If it is wrong, the bus-idle wait before
      each frame fires constantly — which is why that wait is gated on listening being on, and why
      a scan with the monitor running should be timed against one without it.
- [ ] Backward frames decode correctly on a real bus. Every query now goes through the new decoder,
      so this is the highest-risk item in M5 despite being the least visible.
- [ ] Whether the RMT records leading idle before the first edge. The decoder handles both, but
      only hardware settles which happens.
