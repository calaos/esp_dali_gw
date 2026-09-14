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
| `build` | that image | web UI (lint, typecheck, build, bundle budget), `idf.py build`, OTA-slot budget, package, changelog |
| `host-tests`, `lint` | that image | the same two jobs CI runs |
| `publish` | bare runner | verifies `SHA256SUMS`, creates the GitHub release, updates `gh-pages` |

`publish` moves bytes that the container job produced and compiles nothing, which is why it is the
one job outside the image.

## Artefacts

`tools/package-release.sh` produces all of these into `dist/`, and they are all attached to the
release:

| File | Use |
|---|---|
| `esp_dali_gw-merged.bin` | Everything at offset `0x0`. Browser flasher and `tools/flash.sh` |
| `esp_dali_gw.bin` | The app alone. **This is the OTA image** — upload it in the device's own UI |
| `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin` | The individual images, at `0x0`, `0x8000` and `0xf000` |
| `manifest.json` | ESP Web Tools manifest, `chipFamily: ESP32-C6` |
| `SHA256SUMS` | Over every file above |

The merge is `esptool --chip esp32c6 merge-bin -o … @flash_args` from `build/`. esptool in this
toolchain is v5, whose subcommands are hyphenated (`merge-bin`, `write-flash`); the `merge_bin`
spelling from v4 is gone. `@flash_args` carries the offsets and flash settings the build actually
used, so nothing duplicates the partition layout.

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
├── versions.json     # every published version, newest first
├── .nojekyll
├── v1.1.0/
│   ├── manifest.json
│   └── esp_dali_gw-merged.bin
└── v1.0.0/
    └── …
```

`tools/publish-pages.sh` only ever writes those paths; anything else on the branch (a `CNAME`, for
instance) is left alone, and re-running a release produces an identical tree. `versions.json` is
regenerated from the directories that are actually on the branch, so it can never list a version
whose files have been deleted. The page selects the newest by default and offers the rest in a
dropdown.

Old versions are kept. Deleting one is a manual `git rm` on `gh-pages`; the next release's
`versions.json` will simply stop listing it.

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
| New board, from the command line | `tools/flash.sh` with `esp_dali_gw-merged.bin` |
| Gateway already provisioned | OTA upload in its own web UI with `esp_dali_gw.bin`. Keeps Wi-Fi, broker, names, groups and scenes |

Web Serial exists only in Chrome and Edge on the desktop — Firefox and Safari do not implement it,
and no mobile browser does. Flashing never touches the control gear: short addresses, scenes and
groups stay programmed in the fittings.
