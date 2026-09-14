# esp_dali_gw — DALI ⇄ MQTT gateway on ESP32-C6

**Repository:** `github.com/calaos/esp_dali_gw`
**License:** Apache-2.0 (compatible with the Espressif `dali` component and all other dependencies)
**Status:** specification v1.0 — 2026-09-14

---

## 1. Purpose

A standalone Wi-Fi device that bridges one DALI bus (IEC 62386) to MQTT and to an embedded web UI.
It is both a **gateway** (drive DALI control gear from a home-automation system such as Calaos or
Home Assistant) and a **toolkit** (scan the bus, assign short addresses, read/write gear
configuration, send raw frames, observe replies).

Design principles:

1. **One bus owner.** Every DALI frame goes through a single FreeRTOS task (`dali_bus`) fed by a
   command queue. MQTT and HTTP are thin adapters; neither touches the bus directly.
2. **Same payloads everywhere.** The JSON documents exchanged over MQTT and over the REST API are
   identical. One serializer, one parser.
3. **Works with no infrastructure.** The web UI is embedded in the firmware, has no external
   dependency (no CDN), and is fully usable in AP mode with no broker and no Internet.
4. **Boring reliability.** No busy-wait, no `delay()` in callbacks, no bus access from ISRs, every
   long operation reports progress and can be cancelled.

Explicit non-goals for v1: multi-bus, DALI-2 certification, acting as a control *device*
(bus slave), receiving spontaneous input-device events (see §16, v2), TLS on the local HTTP server.

---

## 2. Hardware

| Item | Value |
|---|---|
| MCU board | Waveshare **ESP32-C6-Pico** (ESP32-C6-MINI-1, 4 MB flash, 512 KB SRAM, single core RISC-V @ 160 MHz, Wi-Fi 6 2.4 GHz only) |
| DALI transceiver | Waveshare **Pico-DALI2** stacked on the Pico header |
| DALI TX | **GPIO14** — LOW = bus asserted (shorted), HIGH = bus released. Non-inverting for the Espressif driver (`invert_tx = false`). |
| DALI RX | **GPIO5** — HIGH = bus high (idle), LOW = bus low. `invert_rx = false`. |
| Status LED | On-board RGB LED (WS2812-type) — GPIO **8** by default (verify against Waveshare wiki; make it a Kconfig option). |
| Reset button | On-board BOOT button — GPIO **9** (strapping pin; read only after boot). |
| Bus power | **The Pico-DALI2 has NO bus power supply.** An external DALI PSU (16 V, ≤ 250 mA) must be present on the bus. The firmware must detect and report an unpowered bus (RX permanently low / no start-bit edges). |

Transceiver characteristics (from the Waveshare schematic): galvanic isolation via two EL1018
optocouplers, bridge rectifier (polarity-free bus connection), 600 V MOSFET shorting switch for TX,
5.6 V zener + resistor divider for RX threshold, 1 A fuse. Optocoupler edges are slow (tens of µs);
the RMT decoder rounds to the nearest Te so this is harmless.

All GPIO numbers are **Kconfig options** with the defaults above, and are also overridable at runtime
in the configuration (§6) for other boards.

---

## 3. Toolchain and dependencies

| Component | Source | Notes |
|---|---|---|
| ESP-IDF | **v6.0.x** (pin the minor in CI, e.g. `v6.0.2`) | Warnings are errors by default; keep it that way. Picolibc is the default libc. |
| `espressif/dali` | Component Registry, `^1.1.0` | RMT-based DALI master. Parts 101/102/103/209/303/304. See §7.1 for its limitations. |
| `espressif/mqtt` | Component Registry | esp-mqtt, moved out of IDF in v6.0. |
| `espressif/cjson` | Component Registry | cJSON, moved out of IDF in v6.0. |
| `espressif/mdns` | Component Registry | `esp-dali-gw.local` |
| `esp_http_server` | IDF built-in | REST + SSE + static files + OTA upload |
| `esp_driver_rmt`, `esp_wifi`, `nvs_flash`, `esp_netif`, `esp_event`, `app_update`, `esp_timer` | IDF built-in | |
| RGB LED driver | `espressif/led_strip` from the registry | Status LED |
| Web UI | Node ≥ 20, Vite, Preact, TypeScript | Built in CI, embedded into the firmware image |

All managed components are declared in `main/idf_component.yml` and locked via
`dependencies.lock` (committed).

**If `espressif/dali` does not build under IDF 6 warnings-as-errors, or as soon as listen mode
(§16) is started, vendor it** into `components/dali/` (copy of
`esp-iot-solution/components/dali` at a pinned commit, with a `VENDORED.md` stating the upstream
commit and the local patches). Do not silence warnings globally to work around it.

Verify component names and versions on <https://components.espressif.com> before adding them; do
not guess names.

### 3.1 Development environment — one image everywhere

The toolchain is defined **once**, in `.devcontainer/Dockerfile`, and used by:

1. the **devcontainer** (VS Code / Claude Code / `devcontainer up`),
2. the **CI** (`ci.yml` builds the image from the same Dockerfile, cached by content hash, and
   runs every job inside it),
3. the **release** workflow.

Consequences: an ESP-IDF or Node bump is a one-line change in the Dockerfile; "it builds on my
machine but not in CI" is impossible by construction; a contributor needs only Docker.

| Item | Value |
|---|---|
| Base image | `espressif/idf:v6.0.2` (pinned; bump via the `IDF_VERSION` build arg) |
| Added | Node 22, `clang-format`, `cppcheck`, `picocom`, `usbutils`, `jq`, non-root user `dev` in `dialout`, Claude Code CLI (optional) |
| Workspace | `/workspaces/esp_dali_gw` |
| Caches (named volumes) | ccache, `web/node_modules`, `~/.espressif` |
| Serial | Linux: container is `--privileged` with `/dev` bind-mounted so a board plugged in later is visible at `/dev/ttyACM0`. macOS/Windows: no USB passthrough with Docker Desktop → build in the container, flash from the host with `tools/flash.sh` (esptool) or the browser flasher, or expose the port with `esp_rfc2217_server.py` on the host and set `ESPPORT=rfc2217://host.docker.internal:4000`. |
| Post-create | sources IDF, `npm ci`, pre-fetches managed components, lists serial devices |

To publish the image for faster CI: `release.yml` pushes it to
`ghcr.io/calaos/esp_dali_gw-toolchain:<sha-of-Dockerfile>`; `ci.yml` pulls that tag when it exists
and otherwise builds it. Never use `espressif/esp-idf-ci-action` or a different base image in CI —
that would reintroduce two toolchains.

---

## 4. Architecture

```
                 ┌──────────────┐        ┌──────────────┐
   MQTT broker ◄─┤  mqtt_iface  │        │  http_iface  ├─► browser (REST, SSE, static UI, OTA)
                 └──────┬───────┘        └──────┬───────┘
                        │  cmd (JSON→struct)     │
                        ▼                        ▼
                 ┌───────────────────────────────────────┐
                 │            dali_bus service           │
                 │  command queue ─► bus task ─► driver  │
                 │  gear registry / cache                │
                 │  ESP_EVENT base "DALI_GW" (results,   │
                 │  state changes, progress, bus errors) │
                 └───────────────────┬───────────────────┘
                                     ▼
                      espressif/dali (RMT TX GPIO14 / RX GPIO5)

   app_config (NVS)  ◄──  net_wifi (STA / AP+captive portal / fallback)  ──►  status_led
```

Components (each in `components/<name>/` with `include/`, `src/`, `CMakeLists.txt`, optional
`test/`):

| Component | Responsibility |
|---|---|
| `app_config` | Typed configuration struct, NVS persistence, JSON import/export, validation, defaults, secrets masking. |
| `net_wifi` | Boot state machine (§5), STA connection with backoff, SoftAP + captive DNS, mDNS, RSSI/IP reporting. |
| `status_led` | RGB LED patterns driven by system events. |
| `dali_bus` | The only DALI bus owner. Command queue, request/response with correlation IDs, long operations with progress and cancel, gear registry, error model. Exposes a C API used by both adapters and emits `esp_event`s. |
| `gw_api` | Shared JSON (de)serialization of every command, result, event and model object. Used by both `mqtt_iface` and `http_iface`. Pure logic → host-testable. |
| `mqtt_iface` | esp-mqtt client, topic routing, LWT, retained state, Home Assistant discovery. |
| `http_iface` | `esp_http_server`: REST routes, SSE endpoint, static assets, captive-portal redirect, OTA upload, optional basic auth. |
| `webui` | CMake glue that gzips `web/dist/*` and embeds them (`EMBED_FILES`). |
| `main` | `app_main`: init order, event wiring, watchdog, version banner. |

Tasks and priorities (single core — priorities matter):

| Task | Prio | Stack | Notes |
|---|---|---|---|
| `dali_bus` | 10 | 6 KB | Higher than Wi-Fi/lwIP app tasks so the backward-frame window is served promptly. Only task calling the `dali` driver. |
| esp-mqtt task | 5 (default) | default | Callbacks must return quickly: enqueue and leave. |
| httpd task | 5 | 8 KB | SSE broadcasts are non-blocking (send with timeout, drop slow clients). |
| `net_wifi` | event-driven | — | Runs in the default event loop. |

Inter-component communication uses `esp_event` with a custom base `DALI_GW_EVENT`. Events carry
small POD structs; anything larger (gear list) is pulled from the registry by the consumer.

---

## 5. Network and provisioning

### 5.1 Boot state machine

```
BOOT ─► config has wifi.ssid? ─no─► AP_PROVISIONING
                │yes
                ▼
           STA_CONNECTING ──ok──► STA_CONNECTED (mDNS, MQTT start, LED green)
                │
                │ N failures / T timeout (default 60 s)
                ▼
           STA_FALLBACK_AP  (APSTA: keep retrying STA in background, AP up for recovery)
                │ STA connects
                ▼
           STA_CONNECTED (AP torn down after 30 s grace)
```

- Wi-Fi reconnect uses exponential backoff (1 s → 30 s cap) and never gives up.
- Country code / channel policy from config (default `FR`, world-safe channels 1-11 if unset).
- Hostname default `esp-dali-gw-<id>` where `<id>` = last 3 bytes of the base MAC in lowercase hex
  (`a1b2c3`). Same `<id>` is used in the default MQTT base topic.

### 5.2 AP provisioning and captive portal

- SSID `ESP-DALI-GW-<id>`, WPA2 password default `dali12345` (shown on a label / in README, changeable),
  IP `192.168.4.1/24`, DHCP for 4 clients.
- A minimal **DNS server** answering every A query with `192.168.4.1` (as in the IDF
  `captive_portal` example). Do not add a third-party DNS component if the ~100-line UDP responder
  is sufficient.
- HTTP: any request whose `Host` is not the device IP/hostname, and the OS probe URLs
  (`/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`, `/ncsi.txt`, `/canonical.html`,
  `/success.txt`), get `302 → http://192.168.4.1/`. The UI's first page in AP mode is the **setup
  wizard**.
- The wizard: scan networks (`POST /api/wifi/scan`), pick/enter SSID + password, optional static IP,
  optional MQTT settings, "Save & reboot". After saving, the device reboots into STA mode.
- The full UI (bus tools included) is available in AP mode too — useful for on-site diagnostics
  without any network.

### 5.3 Factory reset

Holding the BOOT button for **5 s** at runtime (LED blinks red during the hold) erases the config
namespace and reboots into provisioning. Also available via `POST /api/factory_reset` and MQTT
`cmd/factory_reset`.

---

## 6. Configuration model

Single JSON document, persisted as one NVS blob (namespace `dali_gw`, key `cfg`, versioned with a
`schema` integer for migrations). Import/export through the API and the UI ("Backup / Restore").

```json
{
  "schema": 1,
  "device": { "name": "DALI gateway", "hostname": "esp-dali-gw-a1b2c3", "timezone": "Europe/Paris" },
  "wifi":   { "ssid": "", "password": "", "static": { "enabled": false, "ip": "", "mask": "", "gw": "", "dns": "" },
              "ap_password": "dali12345", "fallback_ap_timeout_s": 60 },
  "mqtt":   { "enabled": true, "uri": "mqtt://192.168.1.10:1883", "username": "", "password": "",
              "client_id": "esp-dali-gw-a1b2c3", "base_topic": "dali_gw/a1b2c3",
              "keepalive_s": 30, "qos": 0, "retain_state": true,
              "ha_discovery": { "enabled": false, "prefix": "homeassistant" } },
  "http":   { "auth": { "enabled": false, "username": "admin", "password": "" } },
  "dali":   { "tx_gpio": 14, "rx_gpio": 5, "invert_tx": false, "invert_rx": false,
              "poll_interval_s": 30, "scan_on_boot": true, "identify_blink_ms": 500 },
  "led":    { "enabled": true, "gpio": 8, "brightness": 32 },
  "gears":  { "3": { "name": "Kitchen ceiling" }, "12": { "name": "Desk lamp" } },
  "groups": { "0": { "name": "Living room" } }
}
```

Rules:

- Secrets (`wifi.password`, `mqtt.password`, `http.auth.password`, `wifi.ap_password`) are
  **never returned** by the API; they are serialized as `"***"` and a `"***"` value on write means
  "unchanged".
- Validation happens in `app_config` (lengths, ranges, URI scheme `mqtt://`, `mqtts://`, `ws://`,
  `wss://`). Invalid documents are rejected with a field path in the error.
- Changing `wifi.*`, `dali.*gpio*`, `http.auth`, `led.gpio` requires a reboot; the API reply says
  `"reboot_required": true`. Changing `mqtt.*` restarts the MQTT client in place.

---

## 7. DALI bus service (`dali_bus`)

### 7.1 Driver facts that shape the design (espressif/dali v1.1.0, read from source)

- `dali_master_do_transaction()` / `dali_master_do_raw_transaction()` are **blocking** (with
  `vTaskDelay`, not busy-wait) and **not thread-safe** → exactly one calling task.
- RX is armed only after TX completes and for a 25 ms window; a fixed 20 ms inter-frame gap is added
  after every transaction. A query with no reply costs ≈ 60 ms → a 64-address scan ≈ 4 s.
- No bus-idle check, no collision detection, **no passive listening**. Input devices (Part 103) are
  put in quiescent mode by the commissioning helper and are meant to be polled.
- Raw API accepts 2-byte (16-bit) and 3-byte (24-bit) frames with optional send-twice.
- Pass `mem_block_symbols = 0` (auto-detect). ESP32-C6 RMT channels have 48 words, not 64.

### 7.2 Command queue and execution model

```c
typedef struct {
    uint32_t        id;          // correlation id (monotonic, also echoed from API "id")
    dali_cmd_kind_t kind;        // SET_LEVEL, QUERY, CONFIGURE, SCAN, COMMISSION, RAW, IDENTIFY, ...
    dali_target_t   target;      // { SHORT|GROUP|BROADCAST, addr }
    union { ... }   args;
    dali_origin_t   origin;      // MQTT | HTTP | INTERNAL (for logging/ACL)
    QueueHandle_t   reply_q;     // optional: synchronous callers block here (HTTP)
} dali_cmd_t;
```

- Queue depth 32. When full, the adapter replies `bus_busy` immediately (never block an adapter
  task on the bus).
- **Short commands** (set level, single query, configure one parameter) execute inline in the bus
  task; the result is delivered via `reply_q` (if any) and as a `DALI_GW_EVENT_RESULT` event.
- **Long operations** (scan, commission, memory-bank read of all gears, `poll_all`) run as a state
  machine inside the bus task: one DALI transaction per loop iteration, then the queue is checked so
  that a `set_level` from the UI is served between two scan steps. Progress events every step or
  every 250 ms, whichever is rarer. Only one long operation at a time; a second request gets
  `bus_busy` with the running operation name. `cancel` aborts at the next step (and TERMINATEs an
  in-progress commissioning).
- Priority: commands from the queue are served FIFO, except `cancel` and `set_level`/`off`
  (marked `urgent`) which are served before the next long-operation step.
- Every transaction result updates the gear registry cache (`level`, `status`, timestamps) so the
  UI/MQTT state is refreshed without extra queries.

### 7.3 Gear registry

In-RAM array of 64 entries + group/scene metadata, persisted names from config.

```json
{
  "addr": 3, "name": "Kitchen ceiling", "present": true, "last_seen": 1726300000,
  "device_types": [6], "version": "2.0", "dt8": { "caps": 0x30, "tc_min": 153, "tc_max": 370 },
  "level": 128, "level_pct": 50, "on": true,
  "status": { "raw": 4, "gear_failure": false, "lamp_failure": false, "lamp_on": true,
              "limit_error": false, "fade_running": false, "reset_state": false,
              "missing_short_address": false, "power_failure": false },
  "config": { "min": 85, "max": 254, "power_on": 254, "system_failure": 254, "fade_time": 4,
              "fade_rate": 7, "physical_min": 85, "groups": [0, 3],
              "scenes": [254, 0, 128, null, null, null, null, null, null, null, null, null, null, null, null, null] },
  "identity": { "gtin": "4052899000000", "serial": "0000000123", "bank0_version": 1 }
}
```

`null` scene = 0xFF (not programmed). `identity` comes from memory bank 0 and is read lazily on
first scan (`scan` has a `deep: true` option that also reads config and identity).

### 7.4 Operations

| Operation | DALI sequence (Part 102 unless stated) | Notes |
|---|---|---|
| `scan` | For addr 0..63: `QUERY CONTROL GEAR PRESENT`; if yes: `QUERY STATUS`, `QUERY ACTUAL LEVEL`, `QUERY DEVICE TYPE` (+ `QUERY NEXT DEVICE TYPE` loop if 0xFF), `QUERY VERSION NUMBER`. `deep`: + min/max/power-on/fail/fade/groups/scenes/physical-min, memory bank 0 (GTIN, serial), DT8 capabilities & Tc limits when DT8. | Progress `{done, total, found}`. Marks absent gears `present:false` but keeps their names. |
| `commission` | Driver's `dali_commission(mode)` — `mode`: `all` (re-address everything) or `unaddressed` (only gears without a short address). Option `start_addr`. | Requires explicit confirmation flag in the API (`"confirm": true`) for `all`. Followed by an automatic `scan`. |
| `set_short_address` | `DTR0 = (new<<1)|1`; `STORE DTR AS SHORT ADDRESS` (send-twice) to old addr; verify with `QUERY CONTROL GEAR PRESENT` on the new one. | Refuses if new addr is already present. Renames the registry entry. |
| `remove_short_address` | `DTR0 = 0xFF`; `STORE DTR AS SHORT ADDRESS` (send-twice). | |
| `set_level` | DAPC to short/group/broadcast. `level` 0..254 (0 = off with fade), or `level_pct` 0..100 mapped linearly to 1..254 (0 → 0). | `fade_time` optional: sets fade time first, then DAPC. |
| `on` / `off` | `on`: `GO TO LAST ACTIVE LEVEL` (DALI-2, fallback `RECALL MAX LEVEL` if no reply/version 1). `off`: `OFF`. | |
| `cmd` | Any indirect command by name: `up`, `down`, `step_up`, `step_down`, `recall_max`, `recall_min`, `step_down_and_off`, `on_and_step_up`, `go_to_scene:n`, `reset`, `identify` (DALI-2 `IDENTIFY DEVICE`, fallback: blink max/min N times). | Send-twice handled from the command table. |
| `configure` | Any parameter: `min`, `max`, `power_on`, `system_failure`, `fade_time`, `fade_rate`, `extended_fade_time`, `scene[n]` (level or `null` to remove), `group.add[]`, `group.remove[]`. Each: `DTR0 = v` → `STORE DTR AS …` (send-twice) → read back → verify. | Result lists each parameter with `ok` / `mismatch`. |
| `query` | Any query by name or numeric opcode, returns `{reply: int|null, raw_frame}`. | For the toolkit / raw console. |
| `raw` | `frame`: hex string, 4 or 6 hex digits (16/24 bits); `send_twice`; `expect_reply`. Returns the backward frame byte or `null`. | Unrestricted; logged at WARN. |
| `color` | DT8: `{"mirek": n}` or `{"kelvin": n}` or `{"rgb": [r,g,b]}` or `{"rgbwaf": [..]}` via `dali_master_set_color()`. | Only if gear reports DT8. |
| `poll_all` | For each present gear: `QUERY ACTUAL LEVEL` + `QUERY STATUS`. Runs automatically every `dali.poll_interval_s` (0 = disabled) when the queue is idle. | Publishes state only on change. |
| `bus_check` | Sends `QUERY CONTROL GEAR PRESENT` broadcast; reports `powered` (RX idle level high) and whether any reply/collision was observed. | Runs at boot and on demand. |

### 7.5 Error model

Every result carries `ok` and, when false, `error` from this closed set plus a human `message`:

`invalid_arg`, `bus_busy`, `bus_unpowered`, `no_reply`, `tx_failed`, `timeout`, `not_present`,
`address_in_use`, `unsupported` (e.g. DT8 on a DT6 gear), `cancelled`, `internal`.

---

## 8. MQTT API

Base topic `<b>` = `config.mqtt.base_topic` (default `dali_gw/<id>`). QoS from config, retained
where marked. All payloads are JSON (UTF-8). Every command accepts an optional `"id"` (string or
number) that is echoed in the result.

### 8.1 Published by the gateway

| Topic | Retained | Payload |
|---|---|---|
| `<b>/status` | yes (LWT → `offline`) | `{"state":"online","fw":"1.2.0","idf":"6.0.2","ip":"…","rssi":-61,"uptime_s":1234,"mac":"…"}` — refreshed every 60 s |
| `<b>/bus` | yes | `{"powered":true,"busy":false,"operation":null,"progress":null,"gear_count":5,"last_scan":1726300000}` |
| `<b>/gear/<addr>/state` | yes | gear object (§7.3, without `identity`/`config` unless `deep`) — published on every change and after each scan |
| `<b>/gears` | yes | `{"gears":[…compact entries: addr,name,present,level,on,status.raw…]}` |
| `<b>/result/<action>` | no | result object, see §8.3 |
| `<b>/event/progress` | no | `{"operation":"scan","done":17,"total":64,"found":3}` |
| `<b>/event/log` | no | `{"level":"warn","msg":"…"}` for bus errors (rate limited) |
| `<b>/event/rx` | no | v2: passively received frames `{"frame":"A1F3","bits":16,"ts":…}` |

### 8.2 Subscribed by the gateway

| Topic | Payload | Effect |
|---|---|---|
| `<b>/gear/<addr>/set` | `{"level":128}` · `{"level_pct":50,"fade_time":4}` · `{"on":true}` · `{"on":false}` · `{"scene":2}` · `{"cmd":"up"}` · `{"mirek":300}` · `{"rgb":[254,0,0]}` · or a **bare number** `128` (level) · or `ON`/`OFF` | `set_level` / `on` / `off` / `cmd` / `color` on a short address |
| `<b>/group/<n>/set` | same | same on group `n` (0-15) |
| `<b>/broadcast/set` | same | same, broadcast |
| `<b>/cmd/scan` | `{"id":1,"deep":false}` | start scan |
| `<b>/cmd/commission` | `{"id":2,"mode":"unaddressed"\|"all","confirm":true,"start_addr":0}` | start commissioning |
| `<b>/cmd/cancel` | `{}` | cancel running long operation |
| `<b>/cmd/query` | `{"addr":3,"query":"actual_level"}` or `{"addr":3,"opcode":160}` | single query |
| `<b>/cmd/configure` | `{"addr":3,"min":85,"fade_time":4,"scene":{"2":128,"3":null},"group":{"add":[0],"remove":[5]}}` | write config |
| `<b>/cmd/set_short_address` | `{"addr":3,"new_addr":7}` | re-address |
| `<b>/cmd/remove_short_address` | `{"addr":3}` | |
| `<b>/cmd/identify` | `{"addr":3}` | blink |
| `<b>/cmd/raw` | `{"frame":"FF08","send_twice":false,"expect_reply":false}` | raw frame |
| `<b>/cmd/rename` | `{"addr":3,"name":"Kitchen"}` / `{"group":0,"name":"…"}` | registry name (persisted) |
| `<b>/cmd/bus_check` | `{}` | |
| `<b>/cmd/get_gears` | `{}` | republish `<b>/gears` and every `gear/<addr>/state` |
| `<b>/cmd/get_config` / `set_config` | config document (§6) | secrets masked / merged |
| `<b>/cmd/reboot` · `factory_reset` | `{"confirm":true}` | |

Unknown topics/payloads produce `<b>/result/error` with `invalid_arg` and the offending topic.

### 8.3 Result envelope

```json
{ "id": 2, "action": "commission", "ok": true, "duration_ms": 8420,
  "data": { "assigned": 3, "addresses": [0, 1, 2] } }

{ "id": 7, "action": "set_level", "ok": false, "error": "no_reply",
  "message": "gear 3 did not answer QUERY ACTUAL LEVEL", "target": { "type": "short", "addr": 3 } }
```

Long operations publish an immediate `{"ok":true,"data":{"started":true}}` on start, progress on
`event/progress`, and the final result on `result/<action>` when done.

### 8.4 Home Assistant discovery (optional, `mqtt.ha_discovery.enabled`)

For each present gear: `<prefix>/light/esp_dali_gw_<id>_<addr>/config` with `schema: json`,
`brightness: true`, `brightness_scale: 254`, `command_topic: <b>/gear/<addr>/set`,
`state_topic: <b>/gear/<addr>/state`, `state_value_template` mapping `on`/`level`, availability on
`<b>/status`, and `color_temp` / `rgb` support when the gear is DT8. Groups are exposed as lights
too (`…_g<n>`). One `device` block (name, model "ESP DALI GW", sw version, configuration_url =
`http://<ip>/`). Discovery messages are retained and removed (empty payload) when a gear disappears
after a scan.

---

## 9. HTTP API

Served by `esp_http_server` on port 80. JSON bodies and results are **byte-identical to the MQTT
payloads** (§8). Optional HTTP Basic auth (`http.auth`) protects everything except the captive
probes in AP mode.

| Method & path | Body / result |
|---|---|
| `GET /api/info` | `status` object + build info + free heap + `mode: sta\|ap\|apsta` |
| `GET /api/config` · `PUT /api/config` | config document (masked) · merge, returns `{ok, reboot_required}` |
| `POST /api/config/export` · `POST /api/config/import` | full backup (secrets included **only** when `?secrets=1` and authenticated) |
| `POST /api/wifi/scan` | `{"networks":[{"ssid","rssi","auth","channel"}]}` (blocks ≤ 5 s) |
| `GET /api/bus` | bus object |
| `POST /api/bus/scan` · `/commission` · `/cancel` · `/check` · `/raw` · `/query` | same bodies as `cmd/*` |
| `GET /api/gears` · `GET /api/gears/{addr}` | list / gear object |
| `POST /api/gears/{addr}/set` · `POST /api/groups/{n}/set` · `POST /api/broadcast/set` | same as MQTT `set` |
| `POST /api/gears/{addr}/configure` · `/identify` · `/address` · `/remove_address` | |
| `PATCH /api/gears/{addr}` · `PATCH /api/groups/{n}` | `{"name": "…"}` |
| `GET /api/events` | **SSE** stream: `event: gear`, `event: bus`, `event: progress`, `event: result`, `event: log`, `event: rx` (v2). Heartbeat comment every 15 s. Max 3 concurrent clients. |
| `POST /api/ota` | raw `application/octet-stream` firmware; streams into the inactive OTA slot, validates, replies `{ok, next_boot}`; client then calls `POST /api/reboot` |
| `POST /api/reboot` · `POST /api/factory_reset` | `{"confirm":true}` |
| `GET /` and static assets | embedded gzipped files, `Cache-Control: max-age=86400`, ETag = build hash |
| any unknown path in AP mode | `302` to `/` (captive portal) |

Synchronous endpoints (`set`, `query`, `configure`, `raw`) wait for the bus result with a 2 s
timeout and return it in the response body; long operations return `202` with `{started:true}` and
progress arrives on SSE.

---

## 10. Web UI

**Stack:** Vite + Preact + TypeScript, no UI framework beyond a small hand-written CSS (system
font, CSS variables, automatic dark mode). Bundle budget: **≤ 100 KB gzipped total**. No external
resources at runtime. `web/` is the source; `web/dist/` is a build artifact (git-ignored).

**Pages / views:**

1. **Setup wizard** (shown when in AP mode or `wifi.ssid` empty): Wi-Fi scan & credentials →
   optional MQTT → summary → save & reboot, with "connecting…" feedback and a hint about the new
   URL (`http://<hostname>.local/`).
2. **Dashboard:** bus status banner (powered / busy / last scan), all gears as cards (name, addr,
   slider 0-100 %, on/off, status badge, DT8 controls when relevant), "All on / All off",
   groups as collapsible sections. Live via SSE.
3. **Gear detail:** rename, identify, current values, editable config (min/max/power-on/failure
   level/fade time/fade rate, scenes table, groups checkboxes), memory bank identity, "re-address"
   with confirmation, raw query box for that address.
4. **Bus tools:** Scan (normal/deep) with progress bar and cancel; Commissioning wizard (mode
   selection, confirmation for `all`, live progress, result table); Raw console (hex frame, options,
   history of sent frames and replies, and — v2 — live bus monitor).
5. **Settings:** device name/hostname, Wi-Fi (with fallback AP settings), MQTT (with a "test
   connection" button), HA discovery, HTTP auth, DALI GPIO/polarity/poll interval, LED, backup /
   restore, OTA upload with progress, reboot, factory reset.
6. **About / diagnostics:** firmware/IDF versions, uptime, heap, RSSI, recent log lines
   (from the `log` events), link to the GitHub repo.

**Behaviour:** optimistic slider updates with debounce (150 ms) and coalescing (only the last value
is sent), reconnecting SSE with a visible "disconnected" banner, keyboard-accessible controls,
works on a phone in portrait.

---

## 11. Status LED

| State | Pattern |
|---|---|
| AP provisioning | blue, slow breathing |
| STA connecting | yellow blink |
| Connected, MQTT down | green, short double blink every 3 s |
| Connected, MQTT up | green, very dim steady (or off if `led.enabled=false`) |
| Bus unpowered / bus error | red blink |
| Bus activity | brief white flicker (≤ 20 ms) |
| Factory-reset hold | red fast blink |
| OTA in progress | purple breathing |

---

## 12. OTA and partitions

`partitions.csv` for 4 MB flash:

```
# Name,     Type, SubType,  Offset,   Size
nvs,        data, nvs,      0x9000,   0x6000
otadata,    data, ota,      0xF000,   0x2000
phy_init,   data, phy,      0x11000,  0x1000
ota_0,      app,  ota_0,    0x20000,  0x1F0000
ota_1,      app,  ota_1,    0x210000, 0x1F0000
```

- OTA via `POST /api/ota` (web UI) and, optionally, via MQTT `cmd/ota {"url":"https://…"}` using
  `esp_https_ota` with the certificate bundle (behind a config flag, off by default).
- Rollback enabled (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`): the new image marks itself valid only
  after Wi-Fi connects **and** the bus task has run one successful transaction (or 2 minutes elapsed
  with no crash).
- App version from `git describe --tags --dirty` at build time (`PROJECT_VER`), exposed everywhere.

---

## 13. Logging and diagnostics

- `ESP_LOG` tags: `gw`, `cfg`, `net`, `bus`, `mqtt`, `http`, `led`. Default level INFO, `bus` at
  DEBUG prints every frame as `TX 0xA1F3 → RX 0x?? (12 ms)`.
- A ring buffer of the last 100 log lines (WARN+) is exposed on `/api/info` and `event/log`.
- Task watchdog enabled on the bus task and httpd; a stuck bus operation must trip it rather than
  hang silently.
- Core dump to flash disabled (no partition); panic → reboot; reset reason reported in `status`.

---

## 14. Repository layout

```
esp_dali_gw/
├── .devcontainer/                # Dockerfile (single toolchain image), devcontainer.json, post-create.sh
├── CLAUDE.md                     # working conventions for Claude Code (kept short)
├── README.md                     # user-facing: what it is, wiring, flashing, first boot, API links
├── LICENSE                       # Apache-2.0
├── CMakeLists.txt                # project, PROJECT_VER from git
├── sdkconfig.defaults            # common defaults (see §15)
├── sdkconfig.defaults.esp32c6
├── partitions.csv
├── dependencies.lock
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml         # managed components
│   ├── Kconfig.projbuild         # GPIO defaults, AP password default, feature flags
│   └── app_main.c
├── components/
│   ├── app_config/ · net_wifi/ · status_led/ · dali_bus/ · gw_api/ · mqtt_iface/ · http_iface/ · webui/
│   └── (dali/ — only if vendored, see §3)
├── web/                          # Vite + Preact + TS; `npm run build` → web/dist
├── docs/
│   ├── SPEC.md                   # this document
│   ├── MQTT_API.md               # generated/maintained from §8 with full examples
│   ├── HTTP_API.md
│   ├── HARDWARE.md               # wiring, bus PSU, photos, pinout
│   └── adr/                      # architecture decision records (one file per decision)
├── tools/
│   ├── mqtt_cli.py               # small helper: send cmd, wait result, pretty print
│   └── flash.sh                  # esptool merge + flash helper
└── .github/workflows/
    ├── ci.yml
    └── release.yml
```

---

## 15. Build, sdkconfig, CI and release

### 15.1 sdkconfig.defaults (key items)

```
CONFIG_IDF_TARGET="esp32c6"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_ESP_TASK_WDT_EN=y
CONFIG_HTTPD_WS_SUPPORT=n
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
CONFIG_HTTPD_MAX_URI_LEN=512
CONFIG_LWIP_MAX_SOCKETS=16
CONFIG_ESP_WIFI_NVS_ENABLED=y
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_COMPILER_OPTIMIZATION_SIZE=y
CONFIG_COMPILER_STACK_CHECK_MODE_NORM=y
# keep CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS unset (warnings are errors)
```

### 15.2 CI (`.github/workflows/ci.yml`) — on push and PR

0. **toolchain**: compute `hash(.devcontainer/Dockerfile)`; pull
   `ghcr.io/calaos/esp_dali_gw-toolchain:<hash>` or build and push it (`docker/build-push-action`
   with GHA cache). Every job below runs with `container: ghcr.io/calaos/esp_dali_gw-toolchain:<hash>`
   — the **same image as the devcontainer**.
1. **web**: `npm ci`, `npm run lint`, `npm run build`, check bundle size budget, upload
   `web/dist` as artifact.
2. **firmware**: download `web/dist`, `idf.py set-target esp32c6 && idf.py build`; upload
   `esp_dali_gw.bin`, `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin` and a merged
   `esp_dali_gw-merged.bin` (`esptool merge_bin`). Print the firmware size and fail if an app slot
   is > 90 % full. ccache via `actions/cache`.
3. **host-tests**: `gw_api` and `app_config` unit tests (Unity) built for the IDF `linux` target
   where practical; if the linux target proves impractical, a plain CMake + Unity host build of
   those two components is acceptable.
4. **lint**: `clang-format --dry-run --Werror` on C sources (config file committed), `cppcheck`
   (non-blocking at first, blocking once clean).

A `make ci-local` (or `tools/ci-local.sh`) target runs steps 1-4 inside the devcontainer so a
developer can reproduce CI exactly before pushing.

### 15.3 Release (`release.yml`) — on tag `v*`

Builds as above, attaches the binaries + `SHA256SUMS`, generates `manifest.json` for
**ESP Web Tools** and publishes a "Flash from browser" page on `gh-pages` so a user can flash a
board from Chrome without installing anything. Changelog from conventional commits.

---

## 16. Roadmap / milestones

Each milestone ends with: builds warning-free in CI, README updated, manual test checklist in the
PR description, and a git tag.

| # | Milestone | Acceptance |
|---|---|---|
| M0 | **Skeleton** — devcontainer + toolchain image, repo layout, IDF 6 project, managed components, CI building an empty app + empty web UI inside that image, `CLAUDE.md`, docs stubs, LED driver, version banner. | `devcontainer up` + `idf.py build` works from a fresh clone; CI green on the initial commit using the same image. |
| M1 | **Network** — config store, boot state machine, AP + captive portal, STA with fallback, mDNS, web setup wizard, Settings page (Wi-Fi/MQTT/device), factory reset button, OTA upload. | Provision from a phone, device comes up on the LAN at `http://esp-dali-gw-<id>.local/`, survives router reboot, OTA from the UI works. |
| M2 | **Bus core** — `dali_bus` task + queue + registry, `bus_check`, `scan`, `set_level/on/off/cmd`, `query`, `raw`, MQTT client with status/LWT, `gear/<addr>/set|state`, `cmd/*` + `result/*`, `tools/mqtt_cli.py`. | From `mosquitto_pub`: scan finds all gears, dim one gear, state updates retained; UI dashboard shows gears live. |
| M3 | **Toolkit** — commissioning (both modes) with progress/cancel, set/remove short address, `configure` (all params, scenes, groups), deep scan with memory bank identity, identify, rename, Gear detail page, Bus tools page, raw console. | Take a bus of factory-fresh gears to fully addressed, named, grouped and scene-programmed using only the web UI. |
| M4 | **Integration** — Home Assistant discovery, DT8 color, `poll_all`, HTTP auth, backup/restore, docs `MQTT_API.md` / `HTTP_API.md` complete, release workflow + web flasher. | HA shows the gears as lights with brightness/CT; `v1.0.0` released with browser flashing. |
| M5 | **Listen mode (v2)** — vendored `dali` component with a passive RX API (continuous `rmt_receive` when idle, suspended during transactions), bus monitor in the raw console, `event/rx`, bus-idle check before TX, optional Part 103 event decoding (buttons/occupancy) → MQTT events. Contribute the listen API upstream to esp-iot-solution. | Pressing a DALI-2 push-button publishes an MQTT event; the monitor shows every frame on the bus. |

Ideas after M5 (not committed): multiple gateways sharing one base topic namespace, DALI-2
emergency lighting (Part 202) tests, Ethernet variant, ESPHome-style YAML export of the bus layout.

---

## 17. Open decisions (record as ADRs when settled)

- Whether `set_level` should be `urgent` (jump the queue) during a scan — proposed yes.
- `level_pct` mapping: linear (proposed, predictable for users) vs. DALI logarithmic curve.
- HA discovery for groups: always, or only groups that have a name.
- Commissioning `all` from MQTT without `confirm:true` → reject (proposed) vs. accept.