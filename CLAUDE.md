# CLAUDE.md — working rules for this repository

This file is read by Claude Code at the start of every session. Keep it short; the full
specification lives in `docs/SPEC.md` and is the source of truth for *what* to build.

## What this project is

A DALI ⇄ MQTT gateway + DALI toolkit firmware for the Waveshare ESP32-C6-Pico + Pico-DALI2, with
an embedded web UI. ESP-IDF **6.0.x**, C17, RMT-based DALI via the `espressif/dali` managed
component. Read `docs/SPEC.md` before touching anything; if the spec and the code disagree, the
spec wins unless an ADR in `docs/adr/` says otherwise.

## Hard rules

1. **Only `components/dali_bus` calls the `dali` driver.** MQTT/HTTP code enqueues commands and
   consumes events. Never call `dali_master_*` from anywhere else, never from a callback or ISR.
2. **No busy-wait, ever.** No `while (!flag);`, no `esp_rom_delay_us()` above 100 µs, no
   `vTaskDelay` inside MQTT/HTTP callbacks. Block on queues/semaphores with timeouts.
3. **Warnings are errors.** IDF 6 defaults stay. Do not set `CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS`,
   do not add `-Wno-*` flags. Fix the code.
4. **One JSON schema.** Every payload is (de)serialized in `components/gw_api` and reused verbatim
   by both MQTT and HTTP. Never hand-build JSON strings with `snprintf` in an adapter.
5. **Secrets never leave the device** except through the explicit authenticated export path.
   Mask them as `"***"` in every other serialization.
6. **Every long operation** (scan, commission, poll_all) is a resumable state machine in the bus
   task with progress events and a cancel path. No function may hold the bus for more than one
   DALI transaction without checking the queue.
7. **Error handling:** return `esp_err_t`, use `ESP_RETURN_ON_ERROR` / `ESP_GOTO_ON_ERROR` with a
   tag; never `ESP_ERROR_CHECK` outside `app_main` init. Map driver errors to the closed error set
   in SPEC §7.5.
8. **Hardware facts:** TX GPIO14 (LOW = bus asserted), RX GPIO5, no inversion, RGB LED GPIO8,
   BOOT button GPIO9. The bus has **no** internal PSU — an unpowered bus is a normal condition to
   detect and report, not a crash.
9. **Managed components:** verify names/versions on components.espressif.com before adding to
   `main/idf_component.yml`; commit `dependencies.lock`. Known: `espressif/dali ^1.1.0`,
   `espressif/mqtt`, `espressif/cjson`, `espressif/mdns`, `espressif/led_strip`.
10. **No new runtime dependency in the web UI** without a bundle-size check (budget: 100 KB gz).
    No CDN, no web fonts, no analytics.

## Build / flash / test

**Always work inside the devcontainer** (`.devcontainer/`). It is the only supported toolchain and
it is byte-identical to the CI image. If you find yourself installing a tool on the host or with
`apt`/`pip` in an ad-hoc way, stop and add it to `.devcontainer/Dockerfile` instead.

```bash
# firmware (from repo root, inside the devcontainer — IDF env is sourced automatically)
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash monitor          # ESP32-C6-Pico enumerates as USB-JTAG/serial

# web UI
cd web && npm ci && npm run dev               # dev server, proxies /api to a real device (see web/vite.config.ts)
cd web && npm run build                       # required before `idf.py build` (CMake fails with a hint otherwise)

# host tests (gw_api, app_config)
idf.py --preview set-target linux && idf.py build && ./build/esp_dali_gw_tests.elf   # or the CMake+Unity fallback

# formatting
clang-format -i $(git ls-files '*.c' '*.h')
```

A task is not done until `idf.py build` is clean **and** `npm run build` is clean **and** the CI
workflow would pass. When you cannot flash hardware, say so explicitly in the PR/commit message
and list what still needs on-device verification.

## Code style

- C17, 4 spaces, 100 columns, `snake_case`, one component = one public header in `include/`,
  private headers in `src/`. `static const char *TAG` per file. Doxygen comments on public API.
- Prefer `esp_event` for cross-component notification; never expose FreeRTOS handles across
  components except the `dali_bus` reply queue.
- Config structs are plain C structs; JSON only at the edges (`gw_api`, `app_config`).
- TypeScript strict mode, Preact function components + hooks, no class components, no `any`.
- Commits: Conventional Commits (`feat(bus): …`, `fix(http): …`, `docs: …`). One logical change
  per commit. Update `docs/*_API.md` in the same commit as any API change.
- Record non-obvious design decisions as `docs/adr/NNNN-title.md` (context, decision, consequences).

## When in doubt

- Timing on the DALI bus is handled by the driver + the bus task; do not "add a small delay"
  elsewhere to fix a symptom — find the cause.
- If `espressif/dali` needs a patch (IDF 6 warnings, listen mode), vendor it to
  `components/dali/` with a `VENDORED.md` (upstream commit + patch list) instead of monkey-patching.
- Ask before: changing the partition table, changing MQTT topic layout, adding a managed
  component, changing the toolchain image, or deviating from SPEC.md.