# http_iface

The HTTP face of the gateway: REST routes, the SSE event stream, the embedded web UI, the captive
portal and OTA upload. A thin adapter — it never touches the DALI driver, and every payload it
emits or parses goes through `gw_api`, so an HTTP body and the corresponding MQTT payload are the
same bytes. Synchronous endpoints wait on the `dali_bus` reply queue with an explicit timeout
rather than blocking the httpd task indefinitely.

Static assets are served straight out of flash: the bundle is stored gzipped, so a response is the
embedded blob verbatim plus `Content-Encoding: gzip`, and the ETag is the bundle build hash so a
browser keeps the UI cached across reboots.

## Public API

- `http_iface_init()` — start the server and register the routes.
- `http_iface_stop()` — stop it.
- `http_iface_sse_broadcast(event, json)` — push one frame to every SSE client; a client whose
  socket would block is dropped rather than stalling the httpd task.

M0 serves `/api/info` and the static UI. The captive portal, config and Wi-Fi routes and OTA land
in M1; the bus routes and SSE in M2.
