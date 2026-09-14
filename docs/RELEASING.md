# Releasing

`.github/workflows/release.yml` runs on any tag matching `v*`. `ci.yml` deliberately ignores those
tags, so the release workflow is the only gate a tag passes through and it repeats the lint and
host-test jobs itself.

## Cutting a release

```bash
git tag -a v1.0.0 -m 'v1.0.0'
git push origin v1.0.0
```

The tag is the version. `PROJECT_VER` comes from `git describe --tags --dirty --always`, so the
workflow checks out with full history and refuses to continue if `git describe` and the tag name
disagree — that mismatch is what silently ships an image whose `/api/info` reports a bare commit
hash.

`v1.2.3` is a final release. Any other shape (`v1.2.3-rc1`) is marked as a pre-release **and does
not touch the flashing page**, so an rc never becomes the version a stranger flashes.

## What the workflow does

| Job | Where | What |
|---|---|---|
| `toolchain` | runner | Pulls or builds `ghcr.io/calaos/esp_dali_gw-toolchain:<hash of .devcontainer/Dockerfile>` — the same image as the devcontainer and CI |
| `version` | bare runner | Checks the tag against `git describe`, decides pre-release, renders the changelog |
| `web` | that image | Web UI: lint, typecheck, build, bundle budget. Target-independent, so it runs once |
| `build` (matrix: `esp32c6`, `esp32s3`) | that image | `idf.py build` for one target, OTA-slot budget, package |
| `host-tests`, `lint` | that image | the same two jobs CI runs |
| `publish` | bare runner | joins the checksum parts and verifies them, creates the GitHub release, updates `gh-pages` |

`version` and `publish` move bytes and read git; neither compiles anything, which is why they are
the jobs outside the image.

**Two targets, two build trees.** The toolchain image used to pin `IDF_TARGET=esp32c6`, which
silently overrode `idf.py set-target`; it no longer does, so every build states its target. Both
the build directory and the sdkconfig default to a single shared path, so each target is given its
own or the two reconfigure each other's tree:

```bash
idf.py -B build-esp32c6 -D SDKCONFIG=sdkconfig.esp32c6 set-target esp32c6
idf.py -B build-esp32c6 -D SDKCONFIG=sdkconfig.esp32c6 build
```

`sdkconfig.defaults.<target>` is picked up on top of `sdkconfig.defaults` by IDF itself. The S3 has
16 MB of flash and its own partition table, so offsets, the app slot and the merged image all
differ from the C6's — nothing in the release path hardcodes either. `tools/check-app-size.sh
build-<target>` reads the slot size out of the partition table that build actually used, and the
merge reads the offsets out of that build's `flash_args`.

## Artefacts

`tools/package-release.sh <target>` produces one target's payload into `dist/`, and everything
below is attached to the release. **Every name ends in `-<target>` before its extension**, because
a GitHub release is one flat namespace in which `bootloader.bin` means nothing on its own:

| File | Use |
|---|---|
| `esp_dali_gw-merged-<target>.bin` | Everything at offset `0x0`. Browser flasher and `tools/flash.sh` |
| `esp_dali_gw-<target>.bin` | The app alone. **This is the OTA image** — upload it in the device's own UI |
| `bootloader-<target>.bin`, `partition-table-<target>.bin`, `ota_data_initial-<target>.bin` | The individual images, at the offsets that target's `flash_args` gives |
| `manifest-<target>.json` | ESP Web Tools manifest. `chipFamily` is `ESP32-C6` or `ESP32-S3` — the exact spellings esp-web-tools accepts, from its own `const.ts` |
| `SHA256SUMS` | One file, over all twelve images and both manifests |

There is one `SHA256SUMS` but two build jobs, so each writes `SHA256SUMS.<target>` and `publish`
concatenates them and runs `sha256sum --check` over the result. The digests still come from the
machine that produced the bytes; `publish` only joins and verifies. A single-target local run
therefore leaves a `SHA256SUMS.<target>`, not a `SHA256SUMS`.

The merge is `esptool --chip <target> merge-bin -o … @flash_args` from `build-<target>/`. esptool
in this toolchain is v5, whose subcommands are hyphenated (`merge-bin`, `write-flash`); the
`merge_bin` spelling from v4 is gone. `@flash_args` carries the offsets and flash settings the
build actually used, so nothing duplicates the partition layout — which is exactly what lets the
same script package a 4 MB C6 and a 16 MB S3.

## Changelog

`tools/changelog.sh [TAG]` renders the Conventional Commits between the previous `v*` tag and
`TAG`. Breaking changes (`feat!:` or a `BREAKING CHANGE:` trailer) come first; commits whose
subject does not parse land under "Other" rather than being dropped. Run it before tagging to see
what the release notes will say.

## The flashing page

`https://calaos.github.io/esp_dali_gw/` lets anyone flash a board from Chrome or Edge with nothing
installed. It is published from the `gh-pages` branch:

```
gh-pages/
├── index.html        # copied from tools/webflash/index.html on every release
├── versions.json     # every published version, newest first, with the boards it has
├── .nojekyll
├── v1.1.0/
│   ├── esp32c6/
│   │   ├── manifest.json
│   │   └── esp_dali_gw-merged-esp32c6.bin
│   └── esp32s3/
│       ├── manifest.json
│       └── esp_dali_gw-merged-esp32s3.bin
└── v1.0.0/
    └── …
```

`tools/publish-pages.sh` only ever writes those paths; anything else on the branch (a `CNAME`, for
instance) is left alone, and re-running a release produces an identical tree. Which boards a
release has is read off the `manifest-*.json` files in `dist/`, not from a list kept in step with
them.

`versions.json` is regenerated from the directories that are actually on the branch, so it can
never list a version whose files have been deleted:

```json
[
  { "version": "v1.1.0", "builds": [
      { "target": "esp32c6", "manifest": "v1.1.0/esp32c6/manifest.json" },
      { "target": "esp32s3", "manifest": "v1.1.0/esp32s3/manifest.json" } ] }
]
```

The page builds its board picker from the targets that appear there, so a third board needs nothing
but a release that contains it — only the name and the spec line in `BOARDS` in
`tools/webflash/index.html`, and an unlisted target still renders under its own name. Choosing a
version that predates a board says so and hides the install button instead of offering an image
that does not exist.

Old versions are kept. Deleting one is a manual `git rm` on `gh-pages`; the next release's
`versions.json` will simply stop listing it.

### v0.1.0 and the flat layout

`v0.1.0` was published before there was a second board: its manifest and merged image sit at the
top of `v0.1.0/` rather than in a `esp32c6/` subdirectory. **It is left exactly where it is.**
Moving it would break a URL that is already public, and `publish-pages.sh` is not allowed to
destroy anything on the branch. Instead the scan recognises a version whose manifest is flat, reads
the board out of its `chipFamily`, and lists it as a C6-only release; the page then offers the
ESP32-C6-Pico for `v0.1.0` and says the S3 has no build for that version. A per-board subdirectory
wins over a flat manifest if both are ever present, which is what re-cutting such a tag would
leave behind — so the old layout also migrates itself if anyone ever does that.

The merged image is served from `gh-pages` rather than linked to the release asset so that the
binary is same-origin with the page and no CORS header is involved.

**One-time setup, and the order matters.** The `gh-pages` branch does not exist until a release
creates it, and GitHub will not offer a branch that is not there — so the Pages setting cannot be
made first. Cut the first release, then go to Settings → Pages → *Deploy from a branch* →
`gh-pages` / `/ (root)`. The release before that one publishes its artefacts normally; only the
flashing page waits for the setting.

If you would rather have the setting in place beforehand, push an empty branch and select it:

```bash
git switch --orphan gh-pages && git commit -q --allow-empty -m "init pages" \
  && git push -u origin gh-pages && git switch master
```

The release script fetches an existing branch and writes into it, so an empty one is fine.

The page's only remote dependency is the ESP Web Tools module, pinned to an exact version on
unpkg. That is the single documented exception to the project's no-CDN rule (the device's own UI
still has none): esp-web-tools is distributed only as a browser module, and this page is on GitHub
Pages, not on the device. Bump the pin in `tools/webflash/index.html`; the next release ships it.

## Flashing

| Situation | Do this |
|---|---|
| New or spare board, from a desktop | The browser flasher. Erases everything |
| New board, from the command line | `esptool --chip <target> write-flash 0x0 esp_dali_gw-merged-<target>.bin` |
| Gateway already provisioned | OTA upload in its own web UI with `esp_dali_gw-<target>.bin`. Keeps Wi-Fi, broker, names, groups and scenes |

Web Serial exists only in Chrome and Edge on the desktop — Firefox and Safari do not implement it,
and no mobile browser does. Flashing never touches the control gear: short addresses, scenes and
groups stay programmed in the fittings.

`tools/flash.sh` is still single-target: it hardcodes `esp32c6` and `build/`, so it only serves a
C6 built the old way. Use esptool directly for the S3 until it is taught the second target.

The C6-Pico and the S3-Pico are the same board outline and carry the same Pico-DALI2, so the wrong
image is an easy mistake; it damages nothing but the board will not boot. ESP Web Tools reads the
chip on connect and refuses a manifest whose `chipFamily` does not match, so the browser flasher
stops before writing. `esptool` does not check — from the command line the target is yours to get
right.
