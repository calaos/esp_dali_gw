# Web UI

Source of the gateway's embedded web UI: Vite + Preact + TypeScript (strict), hand-written CSS, no
runtime dependency beyond Preact and no external resource at runtime. `npm run build` emits
`web/dist/`, which the firmware build embeds — it is a build artefact and is not committed. The
whole bundle must stay under 100 KB gzipped (SPEC §10).

`npm run dev` serves the UI from your machine and proxies `/api` (including the `/api/events` SSE
stream) to a real device. Set `DEVICE_HOST` to choose which one; it defaults to
`http://esp-dali-gw.local`. Without a reachable device the UI renders its error state, which is
expected.

```bash
tools/docker-run.sh npm --prefix web ci          # install (uses package-lock.json)
DEVICE_HOST=http://192.168.1.42 npm run dev      # dev server on :5173
tools/docker-run.sh npm --prefix web run lint
tools/docker-run.sh npm --prefix web run typecheck
tools/docker-run.sh npm --prefix web run build
```

All npm commands must run inside the toolchain container (`tools/docker-run.sh`), which is the same
image the devcontainer and CI use.

## Without a device

`web/tools/stub-gateway.py` answers the SPEC §9 routes with plausible data and emits a real SSE
stream, so every screen can be exercised on a laptop. It also fakes the things that are hard to
produce on a bench — an unpowered bus, `bus_busy`, a stream that dies mid-scan, a fourth event
listener being refused — through a `/stub/` control plane; the header of the file lists them.

```bash
tools/docker-run.sh npm --prefix web run build
python3 web/tools/stub-gateway.py web/dist 8099   # then open http://localhost:8099/
```
