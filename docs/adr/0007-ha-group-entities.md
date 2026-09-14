# 7. A group gets a Home Assistant entity when it is named or populated

- **Status:** Accepted, 2026-09-14. Settles the last open decision of SPEC §17.

## Context

SPEC §17 leaves open whether Home Assistant discovery should expose groups "always, or only groups
that have a name". Both options are wrong in a common case.

**Always** publishes 16 group entities on every gateway. DALI groups are sparse: a typical
installation uses two or three, and the rest are empty. Fourteen dead lights in someone's Home
Assistant, each of which appears to work and silently drives nothing, is worse than no group support
at all — and they are retained, so they persist until explicitly removed.

**Only named groups** requires the installer to name a group before it appears. That sounds
reasonable until you consider where group membership comes from: a scan reads it off the gear. A bus
commissioned by someone else, or by a different tool, arrives with groups already programmed and no
names anywhere. Under this rule those groups are invisible until the installer discovers, in the
gateway's own UI, that they exist and types a name for each.

## Decision

A group is advertised when **either** it has a persisted name **or** at least one present gear
reports membership in it. Both conditions are cheap: the name is in the configuration, the
membership bitmask comes from the deep-scan `QUERY GROUPS` read that already happens.

When a group loses both — its last member disappears from the bus and it has no name — its
discovery document is removed with an empty retained payload, like a disappearing gear.

## Consequences

- A freshly commissioned bus exposes exactly the groups that are actually wired, with no naming
  step required first.
- Naming an empty group is a way to pre-create an entity deliberately, which is a reasonable thing
  to want and now works.
- A group whose members are all switched off but still present stays advertised: presence, not
  state, is the test. A group whose gear is physically removed disappears after the next scan.
- Membership is only known after a **deep** scan, because a normal scan does not read `QUERY
  GROUPS`. On a gateway that has only ever run a normal scan, groups appear when named and not
  before — the "always" half of the rule is dormant until a deep scan has run. This is a real
  sharp edge and is the price of not querying groups on every scan.
