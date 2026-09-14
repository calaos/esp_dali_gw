# 0002 — One shared JSON schema for MQTT and HTTP

## Status

Accepted, 2026-09-14.

## Context

The same set of operations — set level, scan, commission, configure, query, raw frames, read and
write the configuration — is exposed over MQTT and over the REST API. The specification requires the
two to be interchangeable: HTTP bodies and results are **byte-identical** to MQTT payloads (SPEC
§9), so a user can prototype with `mosquitto_pub` and then hit the same payload against
`/api/...`, and `tools/mqtt_cli.py` and the web UI can share examples.

Two adapters that each format their own JSON will drift. Not immediately and not visibly: a field
added to one path, a number stringified on one side, an error object shaped differently, secrets
masked in the HTTP config export but not in the MQTT one. Every such divergence is a bug that only
shows up in whichever transport the developer was not testing.

## Decision

One component, `gw_api`, owns **all** JSON (de)serialization: every command, every result, every
event and every model object (gear, bus, status, configuration document).

`mqtt_iface` and `http_iface` reuse it verbatim. An adapter's job is transport only — topic or route
matching, auth, QoS or status codes — plus handing an opaque buffer to `gw_api` and shipping back
what it returns.

## Consequences

- **`gw_api` is pure logic.** It has no IDF dependency beyond cJSON, so it builds and its unit tests
  run on the host, where the schema is cheap to test exhaustively.
- **Hand-building JSON in an adapter is forbidden.** No `snprintf` of a payload, not even for a
  trivial `{"ok":true}`. A payload that exists in only one adapter is by definition not part of the
  shared schema.
- **A schema change is a single edit**, and both transports change with it. `docs/MQTT_API.md` and
  `docs/HTTP_API.md` are updated in the same commit.
- **Secrets masking cannot be forgotten by one adapter.** `gw_api` serializes `wifi.password`,
  `mqtt.password`, `http.auth.password` and `wifi.ap_password` as `"***"`, and on write a `"***"`
  value means "unchanged". Because the rule lives in the serializer rather than in each adapter,
  there is no path by which one transport leaks what the other masks. The authenticated export with
  `?secrets=1` is the single explicit exception.
- **Cost: an extra indirection**, and a discipline — `gw_api` must not reach into adapter-specific
  concerns (no topic strings, no HTTP status codes, no `httpd_req_t`). It maps between JSON and the
  typed structs the rest of the firmware uses, nothing more; the moment it knows which transport
  called it, the shared-schema guarantee is gone.
