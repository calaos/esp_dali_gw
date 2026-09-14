# app_config

Typed configuration store (SPEC 6). The whole configuration is one plain C struct persisted as a
single versioned NVS blob (namespace `dali_gw`, key `cfg`), so a change is one atomic write and a
schema bump has one migration point. Nothing here parses or emits JSON — that lives in `gw_api` —
and a corrupt blob falls back to defaults rather than failing, so a bad write cannot brick
provisioning.

## API

- `app_config_init()` — load from NVS, migrate, apply defaults for a blank device.
- `app_config_get()` — read-only pointer to the live configuration.
- `app_config_defaults()` — compiled-in defaults, including MAC-derived identifiers.
- `app_config_validate()` — check a candidate, report the offending dotted field path.
- `app_config_set()` — validate, persist, adopt; reports which subsystems must restart.
- `app_config_merge_secrets()` / `app_config_mask_secrets()` — resolve or hide `***` placeholders.
- `app_config_factory_reset()` — erase the namespace; the caller reboots.
- `app_config_device_id()` — last 3 bytes of the base MAC as lowercase hex.
- `app_config_gear_name()` / `app_config_group_name()` — persisted friendly names.
- `app_config_set_gear_name()` / `app_config_set_group_name()` — persist a friendly name.

Functional in **M1**.
