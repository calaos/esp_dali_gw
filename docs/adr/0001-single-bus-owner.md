# 0001 — A single task owns the DALI bus

## Status

Accepted, 2026-09-14.

## Context

The gateway exposes the same DALI operations over two transports (MQTT and HTTP) plus internal
periodic work (`poll_all`, `scan_on_boot`). The `espressif/dali` driver constrains how those can
reach the bus (SPEC §7.1, read from the v1.1.0 source):

- `dali_master_do_transaction()` and `dali_master_do_raw_transaction()` are **blocking** and **not
  thread-safe**. Two tasks calling them concurrently corrupt the RMT state.
- RX is armed only **after** TX completes, for a **25 ms** window, and a fixed **20 ms** inter-frame
  gap follows every transaction. The backward-frame window is therefore short and must not be
  missed.
- A query with no reply costs ≈ **60 ms**, so a 64-address scan takes roughly **4 s** of solid bus
  occupancy.
- There is **no bus-idle check, no collision detection and no passive listening**. Nothing in the
  driver will notice or recover from two transmitters talking over each other.

A design where each adapter talks to the driver under a mutex would be correct only in the
thread-safety sense: an HTTP request arriving during a scan would block an httpd worker for seconds,
and the 25 ms RX window would be at the mercy of whichever task happened to hold the lock.

## Decision

Exactly one FreeRTOS task, `dali_bus`, owns the bus and is the **only** caller of the
`espressif/dali` driver.

- `mqtt_iface` and `http_iface` are thin adapters. They parse a request into a `dali_cmd_t` and
  enqueue it on the bus task's command queue (**depth 32**). They never touch the driver, and never
  from a callback or ISR.
- Results and state changes come back as `esp_event` notifications on the `DALI_GW_EVENT` base;
  synchronous HTTP callers may additionally pass a `reply_q` and wait on it with a timeout.
- The bus task runs at **priority 10**, above the Wi-Fi/lwIP application tasks.

## Consequences

- **Adapters never block on the bus.** A full queue is answered immediately with `bus_busy`, so an
  httpd worker or the esp-mqtt callback task is never held hostage by bus timing.
- **Long operations must be resumable state machines.** `scan`, `commission` and `poll_all` perform
  one DALI transaction per iteration and check the command queue between iterations, so a
  `set_level` from the UI is served mid-scan instead of waiting ~4 s. No function may hold the bus
  across more than one transaction without checking the queue. Each long operation therefore needs
  explicit progress events and a cancel path; only one may run at a time.
- **Priority 10 keeps the RX window honest.** The backward-frame window is 25 ms after TX; letting
  Wi-Fi or lwIP preempt the bus task there would turn valid replies into `no_reply`. This also means
  the bus task must never busy-wait — it would starve the network stack.
- **Cost: every bus interaction is asynchronous from the adapter's point of view.** Even a
  conceptually trivial `set_level` is enqueue → wait for event or `reply_q`, with correlation IDs to
  match results to requests. Both adapters and the web UI carry that machinery.
