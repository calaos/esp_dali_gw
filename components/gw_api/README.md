# gw_api

The single JSON schema shared by the MQTT and HTTP adapters (SPEC 8, 9; ADR 0002). Both adapters
(de)serialize through this component so their payloads are byte-identical and only one of them can
drift. It knows nothing about DALI opcodes, FreeRTOS or the network: command and query names travel
as strings and are resolved in `dali_bus`, which keeps this component pure logic and testable on the
host.

## API

- `gw_api_status_to_json()` / `gw_api_info_to_json()` — the status object and its `/api/info`
  superset, built from one shared field writer so the two cannot drift.
- `gw_api_err_str()` — wire name of an error from the closed set of SPEC 7.5.
- `gw_api_err_from_esp()` — map a driver-layer `esp_err_t` onto that closed set.
- `gw_api_result_free()` — release the `data` tree owned by a result.
- `gw_api_gear_to_json()` / `gw_api_gears_to_json()` — one gear (optionally deep) or a compact list.
- `gw_api_bus_to_json()`, `gw_api_result_to_json()`, `gw_api_progress_to_json()`.
- `gw_api_config_to_json()` — configuration document; secrets masked unless explicitly exported.
- `gw_api_config_from_json()` — merge a partial configuration document.
- `gw_api_cmd_from_json()` — parse one `cmd/<action>` payload.
- `gw_api_set_from_payload()` — parse a `set` payload, including the bare `ON`/`OFF`/number forms.

The error helpers and the status/info objects are live now; the rest of the serialization surface
is functional in **M2** (configuration documents in **M1**).
