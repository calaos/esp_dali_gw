# mqtt_iface

The esp-mqtt adapter: topic routing, LWT, retained state and Home Assistant discovery (SPEC 8). A
thin adapter by design — payloads are produced and consumed by `gw_api`, commands go to `dali_bus`
through its queue, and broker callbacks enqueue and return so nothing blocks the MQTT task or
touches the bus.

## API

- `mqtt_iface_init()` — start the client if `mqtt.enabled`; a no-op when disabled.
- `mqtt_iface_restart()` — apply a changed `mqtt.*` section by restarting the client in place.
- `mqtt_iface_stop()`
- `mqtt_iface_connected()`
- `mqtt_iface_publish_all()` — republish the retained status, bus, gears and per-gear state topics.

Functional in **M2**; Home Assistant discovery in **M4** — template-schema lights, see
[ADR 0005](../../docs/adr/0005-ha-discovery-template-schema.md).
