# 5. Home Assistant discovery uses the template light schema

- **Status:** Accepted, 2026-09-14. Deviates from SPEC §8.4 on one point.
- **Supersedes:** `schema: json` in SPEC §8.4.

## Context

SPEC §8.4 specifies one discovery document per gear with `schema: "json"`, `brightness_scale: 254`,
`state_topic: <b>/gear/<addr>/state`, `command_topic: <b>/gear/<addr>/set` and a
`state_value_template` mapping the gear object's `on`/`level` onto what Home Assistant expects.

Home Assistant's MQTT JSON light schema has no `state_value_template` (checked against
`homeassistant/components/mqtt/light/schema_json.py`). It parses the state payload itself and reads
the fixed keys `state`, `brightness`, `color_temp` and `color`; unknown keys in a discovery document
are silently dropped (`extra=REMOVE_EXTRA`), so the option would not even produce an error — the
entity would appear and never show a state.

The command direction fails the same way. The JSON schema publishes
`{"state":"ON","brightness":128}`, while `gw_api_set_from_payload()` accepts `level`, `level_pct`,
`on`, `scene`, `cmd` and the colour members. An HA JSON-schema light would therefore be answered
with `invalid_arg` on every press.

So `schema: "json"` cannot work against the payloads of SPEC §8.1–8.2. The alternatives are to
publish a second, HA-shaped state topic and accept HA-shaped commands — which means a second schema
on the wire, against hard rule 4 and ADR 0002 — or to use the one HA schema that maps both
directions with templates.

## Decision

Discovery documents use `schema: "template"`. Everything else in §8.4 stands: retained documents at
`<prefix>/light/esp_dali_gw_<id>_<addr>/config`, `…_g<n>` for groups, availability bound to
`<b>/status`, one `device` block, a stable `unique_id`, and removal with an empty retained payload.

The mapping lives entirely in the templates:

| Direction | Template |
|---|---|
| state | `state_template` reads `on` (`None` when it is `null`); `brightness_template` reads `level` |
| command | `command_on_template` emits `{"mirek":…}`, `{"rgb":[…]}`, `{"level":…}` or `{"on":true}` |
| command | `command_off_template` emits `{"on":false}` |

`brightness_scale` does not exist in the template schema and is not published. It is not needed: the
number Home Assistant carries as brightness **is** the DALI level. Commands clamp to 254 (HA's scale
ends at 255), state passes the level through unchanged, and `level_pct` never appears in a discovery
document — there is exactly one level mapping on the wire, the one ADR 0003 fixed.

## Consequences

- Gear lights report state; group lights have no readback topic in SPEC §8.1 and are therefore
  optimistic in HA, which is honest: a DALI group cannot be queried.
- One service call is one MQTT message and a `/set` payload carries one directive, so a call that
  sets colour *and* brightness applies the colour only. The template tests colour first so that the
  more specific intent is the one that survives.
- Colour state is not reported: the registry has no colour readback. The colour templates render
  empty, which HA ignores, and they start working unchanged if the gear object ever gains `mirek`
  or `rgb`.
- Mired bounds are published as `min_mireds`/`max_mireds` from `dt8.tc_min`/`tc_max` when the scan
  has read them; otherwise HA's defaults apply.
- If Home Assistant ever adds a state template to the JSON schema, moving back is a change of
  document, not of topic layout.
