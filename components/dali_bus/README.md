# dali_bus

The only owner of the DALI bus (SPEC 7, ADR 0001). One FreeRTOS task drains a command queue and is
the sole caller of the `espressif/dali` driver, which is blocking and not thread-safe; adapters
submit commands and consume `DALI_GW_EVENT`. A full queue means `bus_busy` rather than a blocked
adapter, and every long operation is a resumable state machine that checks the queue between
transactions so it can be cancelled or pre-empted by an urgent command. DALI opcode knowledge lives
here, not in `gw_api`: the wire schema carries names as strings and this component resolves them.

## API

- `dali_bus_init()` — create the queue, the registry and the bus task. Does not touch the bus.
- `dali_bus_submit()` — enqueue asynchronously; the outcome arrives as `GW_EVENT_RESULT`.
- `dali_bus_submit_sync()` — enqueue and wait, for the in-band HTTP handlers.
- `dali_bus_cancel()` — ask the running long operation to stop at its next step.
- `dali_bus_get_status()` — powered/busy, current operation, progress.
- `dali_bus_get_gear()` / `dali_bus_get_gears()` — copy registry entries.
- `dali_bus_set_gear_name()` — refresh the cached friendly name after `app_config` persisted it.
- `dali_bus_indirect_name_valid()` / `dali_bus_query_opcode()` — the name tables.

Functional in **M2**; commissioning, `configure` and deep scan in **M3**.
