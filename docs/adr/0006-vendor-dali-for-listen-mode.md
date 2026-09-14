# 6. Vendor `espressif/dali` to add passive listening

- **Status:** Accepted, 2026-09-14.
- **Implements:** SPEC §3 (the conditional vendoring clause) and §16 M5.

## Context

M5 needs the gateway to see traffic it did not send: a DALI-2 push-button's event frame, another
master's commands, and enough of the line state to answer "may I transmit now?". `espressif/dali`
v1.1.0 cannot do any of that, and the reasons are structural rather than a missing convenience
wrapper.

Read from `src/dali_system_components.c` at the pinned commit:

- Both RMT channels are created disabled. `dali_master_do_raw_transaction()` calls `rmt_enable()` on
  TX and RX at the top and `rmt_disable()` on both at the bottom, so **between transactions the RX
  channel is off**. The comment in `dali_new_master_rmt()` says this is deliberate, to save power.
- `rmt_receive()` is only armed **after** `rmt_tx_wait_all_done()` returns, and only when the caller
  passed a non-NULL `result`. A command with no expected reply never arms RX at all.
- The receive window is `DALI_BF_TIMEOUT_MS` = 25 ms, followed by `DALI_IFG_MS` = 20 ms of
  `vTaskDelay`. Outside those 25 ms nothing is listening.
- `dali_decode_backward_frame_byte()` decodes **8 bits only** and rejects longer captures by length
  (`te_len > 26`). A 16-bit or 24-bit forward frame has no representation in the driver.
- `struct dali_master_t` is defined in the `.c` file. The RMT channel handles, the RX buffer and the
  RX queue are unreachable from outside, so a second component cannot arm the channel itself, and
  `rmt_rx_register_event_callbacks()` would need a handle it cannot obtain.

There is no seam. Listening means changing when the RX channel is enabled and who owns it, which is
the innermost part of the driver. SPEC §3 anticipated exactly this and pre-authorised vendoring
rather than patching around it.

The alternative considered was a second RMT RX channel on the same GPIO, owned by `dali_bus`, so the
upstream component could stay untouched. It was rejected: the two channels would both be armed
during a transaction and the listener would capture every frame the master itself sends, which then
has to be filtered by timing — precisely the "usually works" design this component cannot afford.
It also spends a second RMT channel (the ESP32-C6 has four, two of which are RX-capable) for
nothing.

## Decision

`components/dali/` is a copy of `esp-iot-solution/components/dali` at commit `5f9cb98a`
(version 1.1.0), with the local patches listed in `components/dali/VENDORED.md`. The registry
dependency is dropped from `main/idf_component.yml`.

The added API is four functions — `dali_master_listen_start/stop/active` and
`dali_master_bus_idle` — plus `dali_rx_frame_t` and `dali_rx_cb_t`.

Three design points are worth recording because they are the ones a reviewer should push back on:

**Ownership is a mutex held for the whole transaction.** `listen_mux` is the token for the RX
channel. A transaction takes it, cancels whatever the listener had armed, does its work, and gives
it back; the listener takes it only to arm or disarm, never while waiting for a frame. Cancelling
is `rmt_disable()`, which IDF 6's `rmt_rx_disable()` accepts from both the enabled and the running
state and implements as register writes with the interrupt cleared under a spinlock — it does not
wait for the capture to finish. The pause therefore costs microseconds, which matters because
ADR 0001's scan issues a transaction roughly every 60 ms; anything that cost tens of milliseconds
per transaction would turn a 4 s scan into minutes.

**Decoding happens in a task, not the ISR.** The RX-done callback must be in IRAM and is called
from the RMT ISR; it only captures a timestamp and posts a queue entry. The consequence is that the
channel is re-armed by the listener task rather than from the callback, which leaves a gap between
a capture completing and the next one being armed. That gap is bounded by how quickly the task is
scheduled, and the tightest deadline on the bus is a backward frame following a forward frame by
7 Te (≈2.9 ms); subtracting the idle-detection window that ends the capture leaves roughly 1.5 ms.
`DALI_LISTEN_TASK_PRIORITY` defaults to 19 for that reason — above the Wi-Fi and lwIP tasks, whose
bursts are the realistic way to lose those milliseconds. Re-arming from the ISR instead would close
the gap but needs a second capture buffer with no way to detect a torn decode, and `rmt_receive()`
is not in IRAM in this configuration (`CONFIG_RMT_RECV_FUNC_IN_IRAM` is off). A missed frame is a
gap in a passive monitor, never a functional failure, so the simpler, provably race-free option
wins.

**Self-transmitted frames are excluded structurally, not filtered.** The listener's RX channel is
disabled from before `rmt_transmit()` until after the mandatory inter-frame gap, so the master's own
frame cannot reach a listener capture; the echo that the transaction's own receive window does see
is discarded there by frame length, as upstream already did.

`dali_master_bus_idle()` combines an instantaneous read of the RX pin with the time since the last
activity the driver knows about. With the listener stopped, "activity" means only our own
transmissions.

## Consequences

- **We now maintain a fork of a driver we do not own.** Upstream bug fixes and new DALI parts
  arrive as a merge, not a version bump; `dependencies.lock` no longer tells us we are behind.
  `VENDORED.md` carries the pinned commit, a numbered patch list and a re-sync recipe so that
  merge is mechanical, and the files we did not touch are called out explicitly so they can be
  taken verbatim.
- **The backward-frame path changed**, because the vendored decoder replaces upstream's and is
  what `dali_master_do_raw_transaction()` now calls. That is deliberate — one decoder, and the one
  that runs in production is the one under test — but it puts every existing query on new code.
  `components/dali/test/` exercises it on the host over all 8-bit and all 16-bit frames in both
  polarities, and it remains the item to watch on the first bus that is available.
- **The RX channel has two owners and a hand-written handover.** It is documented in the comment at
  the top of `dali_system_components.c` and the two ownership flags are deliberately separate; it is
  still the kind of code that has to be re-read rather than trusted, and it is now on the path of
  every DALI transaction the gateway makes, listening or not.
- **A task and a buffer are spent only when listening is started.** `dali_master_listen_start()`
  creates the task on first use; the queue, mutex, semaphores and the 64-symbol capture buffer are
  allocated with the master.
- **The listen API is meant to go upstream** to esp-iot-solution, as SPEC §16 M5 states. If it is
  accepted, this ADR should be revisited: the fork shrinks back to a plain copy, or disappears and
  the registry dependency comes back. Keeping the patches small and separable is a requirement of
  that plan, not tidiness.
- `components/dali_bus/CMakeLists.txt` should name `dali` rather than `espressif__dali` in its
  `PRIV_REQUIRES`. IDF's component manager resolves the namespaced name to a local component of the
  same base name, so the build is green either way; the rename is for readers.
