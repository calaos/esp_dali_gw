# 3. `level_pct` maps linearly onto the DALI level

- **Status:** Accepted, 2026-09-14. Settles the first two open decisions of SPEC §17.
- **Supersedes:** the illustrative example in SPEC §7.3.

## Context

SPEC §17 leaves the percentage mapping open: linear, or the DALI logarithmic dimming curve.
§7.4 already words the operation normatively — "`level_pct` 0..100 mapped linearly to 1..254
(0 → 0)" — while the registry example in §7.3 shows `"level": 128, "level_pct": 50`, which the
linear formula makes 51 %. The two cannot both be right.

DALI levels are themselves logarithmic: level 254 is 100 % luminous flux, level 1 is 0.1 %, and the
curve between them is exponential. So "50 %" can mean either half the level number or half the
perceived brightness, and the two differ enormously — level 127 is about 3 % of full output.

A second decision rides on this: `set_level` and the reported `level_pct` must agree. If the write
path and the read path use different formulas, a slider set to 50 % reads back as something else
and the UI fights itself.

## Decision

`level_pct` is a **linear function of the DALI level number**, not of luminous flux. Level 0 is
0 %; levels 1..254 map onto 1..100 %. The inverse lives immediately beside it in `gw_api`, and a
host test walks all 101 percentages asserting that pct → level → pct is the identity.

The `"level": 128, "level_pct": 50` example in SPEC §7.3 is wrong under this rule and should read
`51`. This ADR takes precedence over it.

## Consequences

- A user dragging a slider to 50 % gets level 127, which is roughly 3 % of full light output. This
  is surprising the first time and is what every DALI tool does, because the alternative surprises
  differently: a logarithmic slider makes the bottom of the range unusably compressed.
- The mapping is predictable and reversible, which is what an installer setting scene levels wants:
  the number in the UI is the number on the bus, to within rounding.
- Home Assistant is unaffected either way: discovery advertises `brightness_scale: 254` and
  exchanges raw levels, never percentages.
- If a perceptual curve is wanted later it belongs in the **UI**, as a slider response curve over
  the same linear API, not in the wire format. Changing the wire format would silently reinterpret
  every stored scene.
