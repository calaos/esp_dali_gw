# Vendored `espressif/dali`

## Upstream

| | |
|---|---|
| Project | [esp-iot-solution](https://github.com/espressif/esp-iot-solution), `components/dali` |
| Component | `espressif/dali` on <https://components.espressif.com> |
| Version vendored | **1.1.0** |
| Upstream commit | `5f9cb98ae4d0e8153c4b4d1accf471214e5b6fe8` (from the component's `idf_component.yml`, `repository_info.commit_sha`) |
| Licence | Apache-2.0, preserved in `LICENSE` and in every file header |

## Why

`docs/SPEC.md` §3:

> **If `espressif/dali` does not build under IDF 6 warnings-as-errors, or as soon as listen mode
> (§16) is started, vendor it** into `components/dali/` (copy of `esp-iot-solution/components/dali`
> at a pinned commit, with a `VENDORED.md` stating the upstream commit and the local patches). Do
> not silence warnings globally to work around it.

M5 is listen mode. Upstream 1.1.0 enables the RX channel only for the duration of a transaction and
only arms `rmt_receive()` after TX has completed, for a 25 ms backward-frame window; between
transactions the channel is disabled. There is no API to observe the bus, and no hook that could be
bolted on from outside — `struct dali_master_t` is private to `dali_system_components.c`. See
`docs/adr/0006-vendor-dali-for-listen-mode.md`.

The registry dependency is removed from `main/idf_component.yml`; a component in `components/` of
the same name takes precedence, so keeping both would put two copies in the build.

## Local patches

1. **`CMakeLists.txt`: dropped `cmake_utilities`.** Upstream calls `include(package_manager)` /
   `cu_pkg_define_version()`, which only exists to publish a version macro for a registry component.
   Keeping it would pull `espressif/cmake_utilities` back in for a component we no longer fetch.
2. **`CMakeLists.txt`: dropped the pre-5.5 `driver` fallback** and added `esp_driver_gpio` (line
   level for `dali_master_bus_idle()`), `esp_timer` (frame timestamps) and `freertos` to the
   requirements. This project is IDF 6 only.
3. **`CMakeLists.txt`: added `src/dali_frame_decode.c` and `PRIV_INCLUDE_DIRS "src"`.**
4. **Removed `idf_component.yml`, `.component_hash`, `CHECKSUMS.json`, `test_apps/`.** A manifest
   inside `components/` would make the build system resolve upstream's registry dependencies again;
   the hashes describe an archive that no longer exists, and `test_apps/` is upstream's on-device
   pytest harness, which does not run in this project's CI.
5. **New `src/dali_frame_decode.{c,h}`: a general Manchester decoder.** Upstream's
   `dali_decode_backward_frame_byte()` only decodes 8-bit backward frames and rejects anything
   longer by length alone (`te_len > 26`). Listen mode has to decode 16-bit and 24-bit forward
   frames as well. The replacement also fixes three things that mattered once arbitrary bus traffic
   is decoded rather than just replies:
   - a zero-duration RMT symbol is the end-of-capture marker, not a one-tick pulse (upstream's
     `if (n0 == 0) n0 = 1` fabricates a level for it);
   - bit polarity is taken from the level of the trailing idle run instead of from the first pair of
     differing levels, which mis-frames by one Te whenever the capture includes leading idle;
   - a capture that does not end in idle is reported as truncated instead of being decoded as a
     short frame.
   Kept free of ESP-IDF headers (bar the RMT symbol layout) so it can be exercised on the host —
   see `test/`.
6. **`dali_master_do_raw_transaction()` uses the shared decoder** and accepts only an 8-bit result
   as a reply; 16/24 bits is the echo of the frame just transmitted, which is re-armed past exactly
   as before.
7. **`struct dali_master_t`: listen-mode state.** The RX GPIO and its polarity (needed by
   `dali_master_bus_idle()`), an ownership mutex, the listener task, its queue, its own receive
   buffer and receive config, and a last-activity timestamp.
8. **New public API in `include/dali_system_components.h`:** `dali_rx_frame_t`, `dali_rx_cb_t`,
   `dali_master_listen_start()`, `dali_master_listen_stop()`, `dali_master_listen_active()`,
   `dali_master_bus_idle()`.
9. **New listener task in `dali_system_components.c`.** Arms `rmt_receive()` whenever no
   transaction is in flight, decodes captures off the ISR, and delivers them to the user callback.
   The ownership handover between the listener and a transaction is documented in the comment at
   the top of that file.
10. **`dali_master_do_raw_transaction()` pauses and resumes the listener** around the transaction,
    and records the transmission in `last_activity_us`.
11. **The RX-done ISR callback takes the master handle instead of a queue handle** and routes the
    capture to whichever of the two paths armed it.
12. **`dali_del_master()` shuts the listener task down** and frees the new objects.
13. **`Kconfig`: `DALI_LISTEN_TASK_PRIORITY` and `DALI_LISTEN_TASK_STACK`.** Upstream's Part
    102/103/209/303/304 options are untouched.

No `-Wno-*` flag and no `CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS` was needed: upstream 1.1.0 already
compiles clean under IDF 6 warnings-as-errors, so patches 1–13 are all about listen mode.

## Re-syncing with upstream

```bash
git clone --filter=blob:none https://github.com/espressif/esp-iot-solution
git -C esp-iot-solution log --oneline 5f9cb98ae4d0e8153c4b4d1accf471214e5b6fe8..HEAD -- components/dali
git -C esp-iot-solution diff 5f9cb98ae4d0e8153c4b4d1accf471214e5b6fe8..HEAD -- components/dali > /tmp/dali-upstream.patch
```

Apply that diff to `components/dali/`, then re-apply the patch list above where it conflicts. The
sources not listed above (`dali_control_gear.c`, `dali_color_control_dt8.c`, `dali_control_device.c`,
`dali_device_sensors.c`, all headers except `dali_system_components.h`, `Kconfig` below the two new
options) are unmodified and take upstream's version verbatim. Update the version and commit in this
file, run `components/dali/test` and `idf.py build`.

The listen API is meant to go upstream (SPEC §16, M5). If it is accepted, this directory should
shrink back to a plain copy — or disappear, with the registry dependency restored.
