# MQTT API

> **Incomplete.** This document is maintained alongside the implementation: a topic is documented
> here in the same commit that implements it. Topics land in **M2** (status/LWT, bus, gear state,
> `cmd/*`, `result/*`) and the reference is **completed in M4** with full examples and Home
> Assistant discovery. Until then, [SPEC.md §8](SPEC.md#8-mqtt-api) is authoritative — this file
> exists so milestones fill rows in rather than inventing structure.

Base topic `<b>` = `config.mqtt.base_topic` (default `dali_gw/<id>`). All payloads are JSON (UTF-8).
Every command accepts an optional `"id"` (string or number) echoed in the result. QoS comes from the
configuration. HTTP bodies are byte-identical to these payloads — see [HTTP_API.md](HTTP_API.md).

## Published by the gateway

| Topic | Retained | Payload |
|---|---|---|
| | | |

## Subscribed by the gateway

| Topic | Payload | Effect |
|---|---|---|
| | | |

## Result envelope

Every command result carries `id`, `action`, `ok`, and on failure `error` (from the closed set in
SPEC §7.5) plus a human `message`.

TODO: worked examples — a successful `set_level`, a `no_reply` failure, a long operation from
`started` through progress to the final result.

## Home Assistant discovery

Optional, `mqtt.ha_discovery.enabled`. TODO (M4): discovery topic layout and the per-gear and
per-group config payloads.
