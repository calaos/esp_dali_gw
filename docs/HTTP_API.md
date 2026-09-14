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

| Method & path | Body / result |
|---|---|
| | |

## Events (SSE)

`GET /api/events`. TODO: event names, payloads, heartbeat and client-limit behaviour.

## Errors

TODO: status-code mapping for the closed error set in SPEC §7.5.
