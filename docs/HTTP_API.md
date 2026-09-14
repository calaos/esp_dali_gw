# HTTP API

> **Complete for M4, and unverified on hardware.** Every route below is implemented and builds
> clean, but no device has answered any of them: there has been no board, no DALI bus and no broker
> during development. Treat the bus-facing routes in particular as untested against real gear.

Served by `esp_http_server` on port 80. JSON bodies and results are **byte-identical to the MQTT
payloads** — see [MQTT_API.md](MQTT_API.md). Optional HTTP Basic auth (`http.auth`) protects
everything except the captive-portal probe URLs in AP mode.

Synchronous endpoints (`set`, `query`, `configure`, `raw`) wait for the bus result with a 2 s
timeout and return it in the response body. Long operations return `202` with `{"started": true}`
and report progress on the SSE stream.

## Routes

Implemented in M1:

| Method & path | Body | Result |
|---|---|---|
| `GET /api/info` | — | `{status, build, mode, hostname, device_id, free_heap, min_free_heap, reset_reason}`. `status` is nested and byte-identical to the MQTT `<base>/status` payload; `rssi` is absent in AP-only mode rather than reported as `0`. |
| `GET /api/config` | — | The SPEC §6 document with every secret replaced by `"***"`. |
| `PUT /api/config` | Partial or full §6 document | `{"ok":true,"reboot_required":bool,"mqtt_restart":bool}` |
| `POST /api/config/export` | — | Full backup. Secrets are included **only** with `?secrets=1`, and the device logs a warning when they are. Sends `Content-Disposition` so a browser saves a file. |
| `POST /api/config/import` | A backup document | Same as `PUT /api/config`. |
| `POST /api/wifi/scan` | `{}` | `{"networks":[{"ssid","rssi","channel","auth"}]}`. Blocks up to 5 s. Works while the SoftAP is up, which is the provisioning case. |
| `POST /api/ota` | Raw `application/octet-stream` firmware | `{"ok":true,"next_boot":"ota_1"}`. Streamed into the inactive slot; any interruption aborts the handle so a half-written slot is never left bootable. The client calls `POST /api/reboot` afterwards. |
| `POST /api/reboot` | `{"confirm":true}` | `{"ok":true}`, then reboots after the response is flushed. |
| `POST /api/factory_reset` | `{"confirm":true}` | `{"ok":true}`, erases the config namespace, then reboots. |
| `GET /` and static assets | — | Embedded gzipped bundle, `Cache-Control: max-age=86400`, ETag = build hash. A path with no file extension is served the SPA entry point. |
| any path, in AP mode | — | `302` to `http://192.168.4.1/` when the `Host` header is not one of ours, or when the path is an OS connectivity probe. |

### Bus

| Method & path | Body | Result |
|---|---|---|
| `GET /api/bus` | — | `{"powered","busy","operation","progress","gear_count","last_scan"}`. `operation` and `progress` are `null` when idle. |
| `POST /api/bus/scan` | `{"deep":false}` | **202** `{"ok":true,"data":{"started":true}}`. Progress arrives on the event stream; the final result arrives as an `event: result`. A deep scan additionally reads the config block and memory bank 0 of every gear, which is minutes of bus time. |
| `POST /api/bus/commission` | `{"mode":"unaddressed"\|"all","confirm":true,"start_addr":0}` | **202**. `"all"` re-addresses the entire bus and is rejected without `confirm`. |
| `POST /api/bus/cancel` | `{}` | `{"ok":true}`. Stops the running long operation at its next step. |
| `POST /api/bus/check` | `{}` | Result whose `data` is `{"powered":bool,"any_reply":bool}`. |
| `POST /api/bus/raw` | `{"frame":"FF08","send_twice":false,"expect_reply":false}` | Result whose `data` is `{"reply":int\|null}`. `frame` is 4 or 6 hex digits. Unrestricted by design and logged at WARN. |
| `POST /api/bus/query` | `{"addr":3,"query":"actual_level"}` or `{"addr":3,"opcode":160}` | Result whose `data` is `{"reply":int\|null,"opcode":int}`. |

### Gears and groups

| Method & path | Body | Result |
|---|---|---|
| `GET /api/gears` | — | `{"gears":[…]}`, compact entries: `addr`, `name`, `present`, `level`, `on`, `status.raw`. |
| `GET /api/gears/{addr}` | — | The full gear object of SPEC §7.3, including `config` and `identity` when they have been read. |
| `POST /api/gears/{addr}/set` | `{"level":128}` · `{"level_pct":50,"fade_time":4}` · `{"on":true}` · `{"scene":2}` · `{"cmd":"up"}` · `{"mirek":300}` · `{"rgb":[254,0,0]}` · a bare number · `ON` / `OFF` | Synchronous, 2 s budget. |
| `POST /api/groups/{n}/set` · `POST /api/broadcast/set` | same | same |
| `POST /api/gears/{addr}/configure` | `{"min":85,"fade_time":4,"scene":{"2":128,"3":null},"group":{"add":[0],"remove":[5]}}` | Synchronous. `data.parameters` lists every parameter with `ok` or `mismatch`: each one is written and read back, and a partial write is the normal failure. |
| `POST /api/gears/{addr}/identify` | `{}` | **202**. One frame on a DALI-2 gear; a blink sequence otherwise, which is why it is asynchronous. |
| `POST /api/gears/{addr}/address` | `{"new_addr":7}` | Synchronous. Refused with `address_in_use` if a gear already answers there. |
| `POST /api/gears/{addr}/remove_address` | `{}` | Synchronous. |
| `PATCH /api/gears/{addr}` · `PATCH /api/groups/{n}` | `{"name":"Kitchen"}` | Registry and configuration only; never touches the bus. |

A scene of `null` means "not programmed" (0xFF on the wire), which is not level 0.

### Events

`GET /api/events` is a Server-Sent Events stream. Event names: `gear`, `bus`, `progress`,
`result`, `log`. A comment heartbeat is sent every 15 s.

**At most three concurrent clients**; a fourth gets `503` with `error: "bus_busy"` rather than
evicting an existing one. A client that stops reading is dropped rather than being allowed to stall
the server, which runs a single worker task.

### Authentication

When `http.auth.enabled`, every route requires HTTP Basic credentials. The single exemption is the
captive-portal redirect in AP mode: an OS connectivity probe cannot carry credentials, and a `401`
makes the phone report the network as broken instead of opening the portal.

### Writing configuration

`PUT /api/config` is a **merge**, not a replace: members absent from the body keep their stored
value. A secret sent back as `"***"` — which is what `GET /api/config` returned — keeps the stored
secret. A settings form that echoes the masked value therefore leaves credentials untouched, and
only a genuinely new string overwrites one.

The reply distinguishes the two kinds of change: `mqtt_restart` means the client is restarted in
place, `reboot_required` means the change (`wifi.*`, a `dali.*gpio*`, `http.auth`, `led.gpio`) only
takes effect after `POST /api/reboot`.

## Errors

A failure replies `{"ok":false,"error":"<code>","message":"<human text>"}` where `<code>` is from
the closed set of SPEC §7.5. The HTTP status carries the same information so a client can react
without parsing the body:

| Error | Status |
|---|---|
| `invalid_arg` | `400 Bad Request` |
| `not_present` | `404 Not Found` |
| `address_in_use` | `409 Conflict` |
| `unsupported` | `501 Not Implemented` |
| `bus_busy` | `503 Service Unavailable` |
| `timeout`, `no_reply` | `504 Gateway Timeout` |
| everything else | `500 Internal Server Error` |

On a rejected configuration write, `message` is the dotted path of the offending field
(`"mqtt.uri"`, `"dali.rx_gpio"`) so a form can highlight it.
