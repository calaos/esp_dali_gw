# 4. Commissioning is our own state machine, not the driver's `dali_commission()`

- **Status:** Accepted, 2026-09-14.
- **Amends:** SPEC §7.4, which names `dali_commission()` as the implementation.

## Context

SPEC §7.4 says commissioning uses the driver's `dali_commission(mode)`. Reading the component at
the pinned version shows what that function is:

```c
esp_err_t dali_commission(dali_master_handle_t handle, dali_commission_mode_t mode,
                          uint8_t start_addr, uint8_t max_devices, uint8_t *count,
                          int tx_timeout_ms);
```

It runs TERMINATE → INITIALISE → RANDOMISE → the COMPARE/PROGRAM/WITHDRAW binary search →
TERMINATE **in one blocking call**. There is no progress callback and no cancel path; the only
output is the final device count.

That collides with the rest of the design. SPEC §7.2 requires long operations to be resumable state
machines that check the queue between transactions, and design principle 4 in §1 says every long
operation reports progress and can be cancelled. ADR 0001 puts the bus task at priority 10 with a
queue precisely so a `set_level` from the UI is served mid-scan.

The numbers make it concrete: the binary search costs about eight COMPARE rounds per device, each a
transaction of roughly 60 ms including the mandated inter-frame gap. A bus of 64 gears is therefore
on the order of 30 seconds during which the bus task would sit inside one function call — unable to
serve cancel, unable to answer a query, and unable to emit a single progress event. The web UI's
commissioning wizard, which SPEC §10 specifies with "live progress" and a cancel button, could not
be built on it.

## Decision

`dali_bus` implements commissioning itself, as a state machine over
`dali_master_do_transaction()` with `DALI_ADDR_SPECIAL`, advancing one DALI transaction per step
like `scan` already does. The special commands it needs (TERMINATE, INITIALISE, RANDOMISE, COMPARE,
WITHDRAW, PROGRAM SHORT ADDRESS, SEARCHADDRH/M/L) are all reachable through the driver's existing
public API; nothing has to be patched or vendored.

`dali_commission()` is not called.

## Consequences

- Commissioning reports progress per search round and cancels at the next step, like every other
  long operation. A cancel must emit TERMINATE before leaving, or the bus stays in the
  initialise state and ordinary commands are ignored — the cancel path is therefore not merely a
  loop exit and is tested as its own case.
- We own the binary search, including its edge cases: a gear that answers COMPARE but fails to
  withdraw, two gears with identical random addresses, and the 100 ms send-twice window.
- We also own the Part 103 interaction the driver handled for us: input devices must be put in
  quiescent mode before the COMPARE rounds, or their event frames collide with the reply windows.
- More of our code to get wrong, against a driver function that is already written and presumably
  tested. This is the real cost, and it is accepted because the alternative fails a stated
  requirement rather than merely being less elegant.
- If upstream later grows a stepped or cancellable commissioning API, this ADR should be revisited
  and the local state machine dropped.
