# MQTT API

> **Milestone status.** Everything on this page is implemented in **M2** unless a row says
> otherwise. `commission`, `configure`, `set_short_address`, `remove_short_address` and `identify`
> are accepted and routed in M2 but the bus-side operation lands in **M3**; `color` and `poll_all`
> land in **M4**; `event/rx` is **M5 (v2)**. Home Assistant discovery is implemented in **M4** and
> has **not been verified against a live Home Assistant or on hardware** — see the section below.
> [SPEC.md §8](SPEC.md#8-mqtt-api) remains authoritative for the wire schema, with the one
> documented deviation of [ADR 0005](adr/0005-ha-discovery-template-schema.md).

Base topic `<b>` = `config.mqtt.base_topic` (default `dali_gw/<id>`, where `<id>` is the last three
bytes of the base MAC in lowercase hex). All payloads are JSON (UTF-8). Every command accepts an
optional `"id"` (string or number) echoed in the result. QoS comes from the configuration
(`mqtt.qos`), and every topic marked retained below is published with the retain flag only when
`mqtt.retain_state` is true. HTTP bodies are byte-identical to these payloads — see
[HTTP_API.md](HTTP_API.md); both adapters serialize through `gw_api`.

The client starts only when `mqtt.enabled` is true **and** `mqtt.uri` is non-empty. Changing any
`mqtt.*` field restarts the client in place without a reboot.

## Published by the gateway

| Topic | Retained | When | Payload |
|---|---|---|---|
| `<b>/status` | yes | on connect, then every 60 s | device status — same document as the nested `status` block of `GET /api/info` |
| `<b>/status` (LWT) | yes | broker-side, on an ungraceful disconnect | `{"state":"offline"}` |
| `<b>/bus` | yes | on every bus state change, and after a scan or commissioning | bus object |
| `<b>/gear/<addr>/state` | yes | on every cached change of that gear, and for every gear after a scan | gear object (§7.3, without `config`/`identity`) |
| `<b>/gears` | yes | after a scan, after a rename, and coalesced (≤ once per 2 s) after gear changes | compact list |
| `<b>/result/<action>` | no | when a command finishes | result envelope |
| `<b>/result/error` | no | on an unknown topic or an unparseable payload | result envelope with `"error":"invalid_arg"` |
| `<b>/event/progress` | no | on each step of a long operation | progress object |
| `<b>/event/log` | no | on a WARN+ bus log line, **rate limited** | log object |
| `<b>/event/rx` | no | **M5 (v2)** — passively received frames | `{"frame":"A1F3","bits":16,"ts":…}` |
| `<prefix>/light/<object_id>/config` | yes | on connect, after a scan or commissioning, on a rename, and when a gear turns up between two scans | Home Assistant discovery document, or an empty payload to remove the entity |

**Log rate limit.** Five messages pass immediately, then one every two seconds. Bus faults arrive in
bursts — a scan over an unpowered bus produces one line per short address — so the burst makes the
first failure visible at once while the sustained rate keeps a persistent fault reporting without
flooding the broker. Suppressed lines are counted and reported on the device console only; they are
never published late. The full ring buffer is available on `GET /api/info`.

### `<b>/status`

```json
{
  "state": "online",
  "fw": "0.2.0",
  "idf": "6.0.2",
  "ip": "192.168.1.42",
  "rssi": -61,
  "uptime_s": 1234,
  "mac": "a0:b1:c2:a1:b2:c3"
}
```

`rssi` is absent in AP mode, where no STA association exists to measure.

### `<b>/bus`

```json
{
  "powered": true,
  "busy": false,
  "operation": null,
  "progress": null,
  "gear_count": 5,
  "last_scan": 1726300000
}
```

### `<b>/gear/<addr>/state`

```json
{
  "addr": 3,
  "name": "Kitchen ceiling",
  "present": true,
  "last_seen": 1726300000,
  "device_types": [6],
  "version": "2.0",
  "level": 128,
  "level_pct": 51,
  "on": true,
  "status": {
    "raw": 4,
    "gear_failure": false,
    "lamp_failure": false,
    "lamp_on": true,
    "limit_error": false,
    "fade_running": false,
    "reset_state": false,
    "missing_short_address": false,
    "power_failure": false
  }
}
```

`level_pct` is a linear function of the level *number*, not of luminous flux: level 128 is 51 %,
not 50 % ([ADR 0003](adr/0003-level-pct-mapping.md)). A `dt8` block (`caps`, `tc_min`, `tc_max`) is
added when the gear reports device type 8. When
`QUERY STATUS` went unanswered only `"status": {"raw": null}` is published rather than eight `false`
bits, which would read as a healthy gear. The `config` and `identity` blocks of §7.3 are part of the
gear object but are not published on the state topic; read them with `GET /api/gears/{addr}` after a
deep scan.

### `<b>/gears`

```json
{
  "gears": [
    {"addr": 3,  "name": "Kitchen ceiling", "present": true, "level": 128, "on": true,
     "status": {"raw": 4}},
    {"addr": 12, "name": "Desk lamp", "present": true, "level": 0, "on": false,
     "status": {"raw": 0}}
  ]
}
```

A gear that has never answered reports `"level": null`, `"on": null` and `"status": {"raw": null}` —
an unknown value is null, never a plausible zero.

### `<b>/event/progress`

```json
{"operation": "scan", "done": 17, "total": 64, "found": 3}
```

### `<b>/event/log`

```json
{"level": "warn", "msg": "gear 3 did not answer QUERY ACTUAL LEVEL"}
```

## Subscribed by the gateway

The gateway subscribes to exactly four filters: `<b>/gear/+/set`, `<b>/group/+/set`,
`<b>/broadcast/set` and `<b>/cmd/#`. Anything else that reaches it — a malformed address, a topic
below `cmd/` with a further slash, an unparseable payload — is answered on `<b>/result/error`.

| Topic | Payload | Effect |
|---|---|---|
| `<b>/gear/<addr>/set` | see below | act on short address 0-63 |
| `<b>/group/<n>/set` | see below | act on group 0-15 |
| `<b>/broadcast/set` | see below | act on every gear |
| `<b>/cmd/scan` | `{"id":1,"deep":false}` | start a scan (long) |
| `<b>/cmd/commission` | `{"id":2,"mode":"unaddressed"\|"all","confirm":true,"start_addr":0}` | commission (long, **M3**) |
| `<b>/cmd/cancel` | `{}` | abort the running long operation at its next step |
| `<b>/cmd/query` | `{"addr":3,"query":"actual_level"}` or `{"addr":3,"opcode":160}` | one query |
| `<b>/cmd/configure` | `{"addr":3,"min":85,"fade_time":4,"scene":{"2":128,"3":null},"group":{"add":[0],"remove":[5]}}` | write gear config (**M3**) |
| `<b>/cmd/set_short_address` | `{"addr":3,"new_addr":7}` | re-address (**M3**) |
| `<b>/cmd/remove_short_address` | `{"addr":3}` | clear the short address (**M3**) |
| `<b>/cmd/identify` | `{"addr":3}` | blink the gear (**M3**) |
| `<b>/cmd/raw` | `{"frame":"FF08","send_twice":false,"expect_reply":false}` | raw 16/24-bit frame |
| `<b>/cmd/bus_check` | `{}` | report bus power and whether anything answers |
| `<b>/cmd/rename` | `{"addr":3,"name":"Kitchen"}` or `{"group":0,"name":"Living room"}` | persist a friendly name |
| `<b>/cmd/get_gears` | `{}` | republish `<b>/gears` and every `<b>/gear/<addr>/state` |
| `<b>/cmd/get_config` | `{}` | publish the configuration document with secrets masked |
| `<b>/cmd/set_config` | configuration document (§6), possibly partial | merge, validate, persist |
| `<b>/cmd/reboot` | `{"confirm":true}` | reboot |
| `<b>/cmd/factory_reset` | `{"confirm":true}` | erase the config namespace and reboot |

`get_gears`, `rename`, `get_config`, `set_config`, `reboot` and `factory_reset` are answered by the
MQTT adapter itself and never reach the bus. Everything else is enqueued on the bus command queue;
when that queue is full the command is rejected immediately with `bus_busy` rather than blocking.

### `set` payloads

The same payload shapes are accepted on all three `set` topics:

```json
{"level": 128}
{"level_pct": 50, "fade_time": 4}
{"on": true}
{"on": false}
{"scene": 2}
{"cmd": "up"}
{"mirek": 300}
{"rgb": [254, 0, 0]}
```

`level` is 0-254 (0 switches off with a fade); `level_pct` is 0-100 mapped linearly onto 1-254, with
0 meaning off. `cmd` takes any indirect command name: `up`, `down`, `step_up`, `step_down`,
`recall_max`, `recall_min`, `step_down_and_off`, `on_and_step_up`, `go_to_scene:n`, `reset`.
`mirek`/`rgb` require a DT8 gear (**M4**).

Because home-automation systems publish bare values, the following are accepted too and mean the
obvious thing:

```
128
ON
OFF
```

A bare payload carries no `"id"`, so its result is published with `"id": 0`.

### `set_config`

The payload is a configuration document (§6), which may be partial: absent members keep their
current value and a member equal to `"***"` keeps the stored secret. The merge is the same code path
as `PUT /api/config`, so the two adapters cannot diverge.

```json
{"id": 9, "mqtt": {"keepalive_s": 60}, "dali": {"poll_interval_s": 60}}
```

```json
{"id": 9, "action": "set_config", "ok": true,
 "data": {"reboot_required": false, "mqtt_restart": true}}
```

When `mqtt_restart` is true the client is restarted *after* the result has been published, so the
reply always reaches the caller.

## Result envelope

Every command result carries `id`, `action`, `ok`, and on failure `error` (from the closed set in
SPEC §7.5: `invalid_arg`, `bus_busy`, `bus_unpowered`, `no_reply`, `tx_failed`, `timeout`,
`not_present`, `address_in_use`, `unsupported`, `cancelled`, `internal`) plus a human `message`.

A command the adapter answers itself:

```json
{"id": 4, "action": "rename", "ok": true, "duration_ms": 0}
```

`duration_ms` is always present. `data` appears only when `ok` is true; `error` and `message` only
when it is false.

A rejected command — the payload never reached the bus:

```json
{"id": 7, "action": "set_level", "ok": false, "duration_ms": 0, "error": "bus_busy",
 "message": "bus command queue full"}
```

An unknown topic or an unparseable `set` payload. The envelope carries `data` only on success, so
the offending topic is prefixed to the message:

```json
{"id": 0, "action": "error", "ok": false, "duration_ms": 0, "error": "invalid_arg",
 "message": "dali_gw/a1b2c3/gear/99/set: address out of range"}
```

A long operation acknowledges its start, streams progress, and publishes a final result with the
same `id`:

```
→ dali_gw/a1b2c3/cmd/scan            {"id": 12, "deep": false}
← dali_gw/a1b2c3/result/scan         {"id": 12, "action": "scan", "ok": true, "data": {"started": true}}
← dali_gw/a1b2c3/event/progress      {"operation": "scan", "done": 16, "total": 64, "found": 1}
← dali_gw/a1b2c3/event/progress      {"operation": "scan", "done": 64, "total": 64, "found": 2}
← dali_gw/a1b2c3/gear/3/state        { … retained … }
← dali_gw/a1b2c3/gears               { … retained … }
← dali_gw/a1b2c3/result/scan         {"id": 12, "action": "scan", "ok": true, "duration_ms": 4120}
```

A failure reported by the bus:

```json
{"id": 7, "action": "query", "ok": false, "duration_ms": 62, "error": "no_reply", "message": "",
 "target": {"type": "short", "addr": 3}}
```

> Results for commands executed by the bus are rebuilt from the `DALI_GW_EVENT` result event, which
> carries only the correlation id, action, outcome, target and duration. Such results therefore have
> an empty `message` and no `data`; the synchronous HTTP endpoints return the full envelope. This is
> an asymmetry of the M2 event payload, not a schema difference.

## Command-line helper

`tools/mqtt_cli.py` sends one command, waits for the result with the matching `id` and pretty-prints
it. It needs `paho-mqtt` (`pip install --user paho-mqtt`), which is deliberately not part of the
devcontainer image.

```bash
tools/mqtt_cli.py --host 192.168.1.10 scan
tools/mqtt_cli.py set 3 --level 128
tools/mqtt_cli.py set group:0 --off
tools/mqtt_cli.py set broadcast --level-pct 50 --fade-time 4
tools/mqtt_cli.py query 3 actual_level
tools/mqtt_cli.py raw FF08 --send-twice
tools/mqtt_cli.py watch                      # print everything under <b>/#
```

`--base-topic` defaults to `dali_gw/+`; the concrete base topic is resolved from the retained
`<b>/status` message, so a single gateway on the broker needs no extra flag. `--host`, `--port`,
`--username` and `--password` cover the rest, and the exit status is 0 on `"ok": true`, 1 on a
failed result and 2 on a timeout.


## Home Assistant discovery

Optional, off by default. `mqtt.ha_discovery.enabled` turns it on and `mqtt.ha_discovery.prefix`
(default `homeassistant`) says where the documents go. Every gear and group is published as a
**light**:

| Entity | Config topic | `unique_id` |
|---|---|---|
| short address `<addr>` | `<prefix>/light/esp_dali_gw_<id>_<addr>/config` | `esp_dali_gw_<id>_<addr>` |
| group `<n>` | `<prefix>/light/esp_dali_gw_<id>_g<n>/config` | `esp_dali_gw_<id>_g<n>` |

`<id>` is the same six hex digits as in the default base topic. The documents are **always
retained**, whatever `mqtt.retain_state` says: Home Assistant reads them when *it* starts, which is
usually not when the gateway publishes them.

> **Schema.** The documents use Home Assistant's **template** light schema, not the `schema: json`
> of SPEC §8.4. The JSON schema has no state template, and it commands with its own
> `state`/`brightness` keys, which `<b>/gear/<addr>/set` does not accept — it would break in both
> directions. [ADR 0005](adr/0005-ha-discovery-template-schema.md) has the full reasoning.

### The document

A present DT8 gear whose Tc limits have been read, on a gateway at `192.168.1.42`:

```json
{
  "schema": "template",
  "name": "Kitchen ceiling",
  "unique_id": "esp_dali_gw_a1b2c3_3",
  "command_topic": "dali_gw/a1b2c3/gear/3/set",
  "command_on_template": "{% if color_temp is defined %}{\"mirek\": {{ color_temp }}}{% elif red is defined %}{\"rgb\": [{{ [red, 254] | min }}, {{ [green, 254] | min }}, {{ [blue, 254] | min }}]}{% elif brightness is defined %}{\"level\": {{ [brightness, 254] | min }}}{% else %}{\"on\": true}{% endif %}",
  "command_off_template": "{\"on\": false}",
  "brightness_template": "{{ value_json.level | default('', true) }}",
  "state_topic": "dali_gw/a1b2c3/gear/3/state",
  "state_template": "{% if value_json.on is none %}None{% elif value_json.on %}on{% else %}off{% endif %}",
  "availability_topic": "dali_gw/a1b2c3/status",
  "availability_template": "{{ value_json.state }}",
  "qos": 0,
  "color_temp_template": "{{ value_json.mirek | default('', true) }}",
  "min_mireds": 153,
  "max_mireds": 370,
  "red_template": "{{ (value_json.rgb | default([]))[0] | default('', true) }}",
  "green_template": "{{ (value_json.rgb | default([]))[1] | default('', true) }}",
  "blue_template": "{{ (value_json.rgb | default([]))[2] | default('', true) }}",
  "device": {
    "identifiers": ["esp_dali_gw_a1b2c3"],
    "name": "DALI gateway",
    "model": "ESP DALI GW",
    "sw_version": "0.4.0",
    "configuration_url": "http://192.168.1.42/"
  }
}
```

About 1.3 kB for a DT8 gear, 0.9 kB without colour. Differences for the other cases:

- **Not DT8:** no `color_temp_template`, no mireds, no `red`/`green`/`blue_template`. The entity is
  on/off + brightness.
- **DT8 without Tc limits:** `color_temp_template` is published but `min_mireds`/`max_mireds` are
  not, and Home Assistant applies its own defaults. The limits appear once a deep scan has read
  `dt8.tc_min`/`tc_max`.
- **Group:** `command_topic` is `<b>/group/<n>/set` and there is **no** `state_topic` — a DALI group
  cannot be queried, so SPEC §8.1 has no group state topic and the entity is optimistic in Home
  Assistant. `brightness_template` is still published, because its presence is what gives the entity
  a brightness slider.
- **`name`** is the persisted friendly name, or `Gear <addr>` / `Group <n>` when there is none.
- **`configuration_url`** is omitted while the device has no IP address.

### Levels, not percentages

`brightness_template` reads `level` and `command_on_template` writes `level`, so the number Home
Assistant calls brightness **is** the DALI level, 0-254. `level_pct` appears nowhere in a discovery
document: there is one level mapping on the wire and it is the linear one of
[ADR 0003](adr/0003-level-pct-mapping.md). Home Assistant's own scale ends at 255, so the command
template clamps to 254 — dragging a slider to the top gives level 254 and the state comes back 254.

`brightness_scale: 254` from SPEC §8.4 is a JSON-schema option and is not published; the template
schema has no such option and needs none.

### When the documents are published

| Trigger | What is published |
|---|---|
| MQTT connect | every document, after the retained state topics |
| a scan or commissioning finishes | every document, plus removals |
| `cmd/rename` | that one gear or group |
| a gear turns up between two scans | that one gear |

Nothing is published on a level or status change — a discovery document does not depend on the state
of the gear, and republishing 64 of them every time someone dims a light would be the main MQTT
traffic on the gateway.

Home Assistant restarting needs nothing from the gateway: the documents are retained and the broker
replays them. The republish on connect is for the two cases where the broker's copy is wrong or
gone — a broker that was restarted without a persistent store, and a changed IP address or base
topic inside the document.

### Removal

A discovery document is removed by publishing an **empty retained payload** to the same config
topic. This happens when:

- a gear that had an entity is no longer `present` after a scan;
- a group loses its last known member and its name;
- `mqtt.ha_discovery.prefix` changes — the old prefix is emptied before the new one is used;
- `mqtt.ha_discovery.enabled` goes false, which is treated as an empty prefix.

The gateway also runs **one blind removal pass**, over every address and group it has not itself
advertised, the first time it reconciles after a scan has completed. That is what clears documents
left retained by a previous run of the gateway; afterwards it knows exactly what it published and
removes only that. Before the first scan nothing is removed — the registry is empty at boot and a
gateway that merely rebooted must not drop its own entities.

> Consequence worth knowing: disabling discovery *while the gateway is connected* removes the
> documents. Disabling it and rebooting does not — the gateway no longer knows what it published.
> Clear them by re-enabling discovery, letting a scan finish, then disabling it again.

### Known limitations

- **Colour is write-only.** The gear object has no colour readback, so `color_temp_template` and the
  RGB templates render empty and Home Assistant keeps the value it last sent. The templates are
  written against `value_json.mirek` and `value_json.rgb` and will start reporting unchanged if the
  registry ever gains those fields.
- **One directive per call.** A `/set` payload carries one directive, so a service call that sets
  colour *and* brightness applies the colour only; the template tests colour first deliberately.
- **Groups have no state**, are optimistic, and are advertised only once a deep scan has read their
  membership bits or someone has named the group with `cmd/rename`.
- **Not verified.** The documents have been checked field by field against the Home Assistant
  template-light schema and every template has been rendered against real payloads, but no live
  Home Assistant, broker or gear has been part of that.

## Out of scope

- **Passive listening** and `<b>/event/rx` — **M5 (v2)**. The `espressif/dali` driver has no
  bus-idle detection and no listen mode, so nothing the gateway did not send is observable today;
  input devices (Part 103) are polled, not received.
- Acting as a control *device* (bus slave), multi-bus, and TLS to the local HTTP server (SPEC §1).
- Home Assistant entities other than lights: no scene, switch or sensor entities are published.
