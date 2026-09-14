# webui

CMake glue that turns `web/dist` into part of the firmware image. At configure time
`gen_assets.py` gzips every built asset into the build directory, generates a C table mapping
request paths to the embedded blobs and their content types, and hands the list to `EMBED_FILES`.
`http_iface` serves the result without knowing any of this. A missing or empty `web/dist` fails the
build with the command to run rather than producing a firmware that 404s its own UI.

Gzip is written with `mtime=0` so the compressed bytes are reproducible: the build hash — used
verbatim as the HTTP ETag — only changes when an asset actually changes.

## Public API

- `webui_find(path)` — asset for a request path; `/` resolves to the SPA entry point. NULL when
  unknown, leaving the captive-portal-redirect-or-404 decision to the caller.
- `webui_index()` — the SPA entry point, for client-side routing fallbacks.
- `webui_build_hash()` — build hash of the bundle, used as the ETag.

Functional since M0.
