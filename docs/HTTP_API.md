# HTTP API

> **Incomplete.** This document is maintained alongside the implementation: a route is documented
> here in the same commit that implements it. Routes land in **M1** (info, config, Wi-Fi scan, OTA,
> reboot, factory reset, static assets) and **M2** (bus, gears, SSE), and the reference is
> **completed in M4**. Until then, [SPEC.md §9](SPEC.md#9-http-api) is authoritative — this file
> exists so milestones fill rows in rather than inventing structure.

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

Landing in M2: `/api/bus*`, `/api/gears*`, `/api/groups*`, `/api/broadcast/set`, `/api/events`.

### Writing configuration

`PUT /api/config` is a **merge**, not a replace: members absent from the body keep their stored
value. A secret sent back as `"***"` — which is what `GET /api/config` returned — keeps the stored
secret. A settings form that echoes the masked value therefore leaves credentials untouched, and
only a genuinely new string overwrites one.

The reply distinguishes the two kinds of change: `mqtt_restart` means the client is restarted in
place, `reboot_required` means the change (`wifi.*`, a `dali.*gpio*`, `http.auth`, `led.gpio`) only
takes effect after `POST /api/reboot`.

## Events (SSE)

`GET /api/events`. TODO: event names, payloads, heartbeat and client-limit behaviour.

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
(`"mqtt.uri"`, `"led.brightness"`) so a form can highlight it.
