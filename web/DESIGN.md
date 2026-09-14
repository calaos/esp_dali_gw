# Design system

The token layer in `web/src/styles.css` is the whole design system. Every screen from M1 onwards
consumes those custom properties and adds nothing to the colour, type, spacing or motion
vocabulary. This file records where the vocabulary came from and how it was checked.

## What the UI is

A control panel for one DALI bus, served by the device over its own Wi-Fi. It is read standing on a
ladder, on a phone, often in a dim hallway next to an electrical cabinet, by someone who wants to
know whether a command landed. It is instrumentation, not a product page: dense, left-aligned, no
decoration that does not carry state.

## Calaos identity — what is sourced

Calaos has no published brand guide. These values come from reading the project's own artefacts:

| Value | Source |
|---|---|
| `#3ab4d7`, `#8cd3e7` | Dominant pixel colours of the logotype `img/logo_full@2x.png` in [`calaos/calaos_mobile`](https://github.com/calaos/calaos_mobile) |
| `#3ab4d7` as the app accent | Most-used colour in the QML of `calaos_mobile` (69 occurrences, ahead of every other hue) |
| `#38b0d3` accent, `#4caf7d` success, `#e0a356` warning, `#e05656` danger, near-black surfaces `#111`→`#3c3c3c`, `#f2f2f2` text | `app/src/styles/theme.css` in [`calaos/calaos-web-app`](https://github.com/calaos/calaos-web-app), the current-generation Calaos web UI, which states "Dark theme, cyan accent" |
| `#80dcf3`, `#2e8ba9` on a dark ground | Stylesheet of [calaos.fr](https://calaos.fr/en/) (`css/style.css`) |
| Ubuntu as the Calaos typeface | `--font-sans` in `calaos-web-app`, which calls it "the Calaos face"; `calaos_mobile` bundles its own `calaos_text.ttf` |
| Radii of 4–5 px | `radius:` values across `calaos_mobile`'s QML |
| 120 ms press feedback | `--press-duration` in `calaos-web-app` |
| Dark-first | Both the mobile app and the web app are dark-only |

The identity is consistent on one point and thin on the rest: **a cyan accent in the `#38b0d3`–
`#3ab4d7` range on a dark, near-neutral ground, with green/amber/red status colours**. The
logotype's one distinctive structural idea is that each letterform is split horizontally into a
pale upper half and a saturated lower half.

## What is interpretation

Stated plainly, because none of this exists in any Calaos artefact:

- **The light theme.** Calaos ships dark only. The light ramp here is mine. It keeps a cool cast
  (neutrals pulled towards the accent's hue) rather than the warm paper that a default light theme
  reaches for, so the two themes read as one family.
- **The exact hexes.** No Calaos value is copied verbatim. The cyan is re-tuned per theme to clear
  AA (`#0b7391` light, `#45c0e0` dark); the greens, ambers and reds are re-derived from the Calaos
  ones for the same reason. `#3ab4d7`/`#8cd3e7` are the only literal Calaos values, kept for the
  brand mark where contrast rules do not apply.
- **Two extra semantic states.** Calaos defines ok/warn/danger. A DALI bus also needs *offline /
  absent* (a short address that answers nothing) and *busy* (a scan or commissioning run in
  flight). Busy resolves to the accent: cyan already means "working" everywhere in Calaos.
- **The type scale, spacing scale and motion set.** Calaos publishes a partial set; this extends it.

## Decisions

**Dark is the theme the design is drawn for**, because the device is used in situ at night and
because both Calaos UIs are dark. Light is nonetheless the layer that defines every token on bare
`:root`, so no colour has its only definition inside a media query. Dark is applied twice — under
`@media (prefers-color-scheme: dark)` guarded by `:root:not([data-theme='light'])`, and under
`:root[data-theme='dark']` — so the automatic behaviour and the future Settings override both work,
in both directions.

**No web fonts, and the Calaos face for free where it exists.** `Ubuntu` leads `--font-sans` and
`Ubuntu Mono` leads `--font-mono`. On Calaos OS and on the Linux wall panels that is the real
Calaos typeface at zero bytes; everywhere else it falls through to `system-ui`. Personality has to
come from the scale and the treatment instead of the family, so the scale is tight (1.2 ratio,
six steps) the way an instrument's is, not airy.

**Monospace is structural, not decorative.** DALI's vocabulary is the 6-bit short address, 0–63,
plus hex frames, IPs and versions. `.mono` sets `tabular-nums` so a column of those aligns
vertically and scans. It is never used for labels.

**Status lives on a panel's leading edge, not in its fill.** `.panel--rail` draws a 3 px bar
coloured by `--rail`. A tinted card background is the obvious move and it is wrong here: it washes
out at arm's length, and a dashboard of 64 gears tinted five different ways is confetti. A rail
reads across a room, stacks cleanly in a grid, and leaves the card's fill free to mean "surface".
The rail is also the logotype's horizontal split rotated upright — the same device, reused.

**Status is never colour alone.** `.badge` always carries its own word; `.dot` only ever sits
inside one.

**Depth is not a drop shadow in dark mode.** `--shadow-1` resolves to `none` under dark and
elevation comes from `--c-surface` / `--c-raised` plus the hairline. The same soft grey shadow under
every card is the tell of a template, and in dark it just muddies the edge.

**Two border tokens, deliberately.** `--c-border` is a hairline between two filled surfaces; the
fill difference already marks that edge, so WCAG 1.4.11 does not apply and it stays quiet.
`--c-border-strong` is for anything that is the *only* boundary of a control — input outlines,
slider rails, checkbox frames — and clears 3:1 in both themes. Use the wrong one and a slider rail
becomes invisible.

**Touch and motion.** `--control-h` is 44 px and every button and input inherits it.
`--press-scale` gives the tactile press Calaos uses; `prefers-reduced-motion` collapses all three
duration tokens to 1 ms and sets the press scale to 1, on top of a global transition/animation
override.

## Screens (M1)

Three views behind a hash router: the setup wizard, Settings and About. Dashboard, gear detail and
bus tools (SPEC §10 views 2-4) land in M2/M3; the nav is built from the `NAV` array in
`src/router.ts`, so adding them is one entry each.

**One alignment rule across read and write.** `.data-list` already set it — label left in muted
small type, value right — and `.field` reuses the same two-column grid for form rows. A Settings
row and an About row are the same object in write and read mode, so the two screens scan
identically instead of looking like two different products. Below 44rem both collapse to one
column, which is also where the label column would start squeezing the control.

**The edge marker is the only structural device, everywhere.** `.panel--rail` marks status, the
current `.nav__link` carries the same 3 px bar on its bottom edge, a completed `.steps__item` fills
its bar with the accent, and the selected `.netlist__row` gets it as an inset shadow on its leading
edge. "This one" looks the same whatever it is attached to. Nothing else was invented to mean it.

**Restart flags are per section when the whole section restarts.** SPEC §6 says Wi-Fi, the DALI
GPIOs, HTTP auth and the LED GPIO need a reboot. Badging every Wi-Fi field individually produced
six identical badges in one panel, which reads as decoration; `Section` takes the flag once, and
the per-field badge is kept only for the mixed sections (DALI pins among non-pin settings, LED pin
among LED settings). The device also reports `reboot_required` after the write, but the point is to
say so *before* the user commits.

**Confirmation is inline, never a modal.** `ConfirmButton` expands in place into a question and two
buttons; `TypeToConfirm` additionally requires the word `RESET` before the destructive button
enables. No dialog means no focus trap to get wrong, no scroll lock, and the question stays under
the thumb on a phone. The factory-reset copy names what is lost — credentials, broker, fitting
names — rather than asking "are you sure?".

**Unsaved work follows the page.** `.savebar` sticks to the bottom of the viewport while any leaf
differs from the loaded document. It counts the changes and switches its rail from busy to warn,
and its button from "Save" to "Save and restart", when one of the changed paths needs a reboot.

**The handoff screen is where the wizard spends its attention.** After "Save and restart" the page
is about to lose the device on purpose, which is the single most confusing moment in the product.
The new address is set at `--text-xl` in the mono face as the hero, the copy names the failure
before the user sees it ("an error here is the handover finishing, not something going wrong"), and
the two follow-up actions are numbered because they genuinely are a sequence. A probe keeps polling
the old origin so that a reconfigure which stayed on the same network reports success; its silence
is the documented, expected outcome, not an error state.

**Secrets are a UI state, not just a value.** The device sends `***` and treats `***` on a write as
"unchanged" (SPEC §6). `SecretField` therefore shows "Stored on the device" plus a Change button
rather than a password box full of fake dots, and the write body is a leaf-level diff against the
loaded document (`patchFrom`), so an untouched field is simply absent from the request. There is no
code path that can turn "untouched" into `""`.

**Progress is only ever real.** `.meter` animates because a transfer is actually advancing; the
indeterminate variant sweeps only while there is genuinely nothing to divide (a scan in flight, an
image being verified). Nothing else in the UI moves on its own: no entrance animations, no hover
transitions on cards.

## Additions to the vocabulary

No new colour, type, spacing or motion token. New classes only, all composed from existing tokens:

| Class | Role |
|---|---|
| `.stack`, `.row`, `.row--end` | gap-based flow containers |
| `.section__head`, `.section__lede` | a panel heading with a trailing action, and its lede |
| `.nav`, `.nav__link`, `.nav__link.is-current` | the sticky section bar and its edge marker |
| `.field`, `.field-grid`, `.field__control`, `.field__hint`, `.field__flag`, `.field__row`, `.field__unit`, `.field__stored`, `.field__spacer`, `.field--check` | the form row and its parts |
| `.check` | a checkbox and its own label as one 44 px target |
| `.file-button`, `.file-button.is-disabled` | a `.btn` label wrapping a hidden file input |
| `.notice`, `.confirm` | inline message and inline confirmation, both driven by `--status` |
| `.meter`, `.meter__track`, `.meter__track--pending`, `.meter__fill`, `.meter__value` | progress |
| `.steps`, `.steps__item`, `.steps__mark` | the wizard's step rail |
| `.netlist`, `.netlist__row`, `.netlist__name`, `.netlist__lock`, `.signal` | the scan result list |
| `.savebar`, `.savebar__text` | the sticky unsaved-changes bar |
| `.handoff__url`, `.handoff__steps` | the post-save handoff screen |

`.btn--sm` exists in the token layer but is not used on any touch target in M1: every button and
input in these screens is at least `--control-h` (44 px). Keep it for the dense gear lists in M2.

The two icons (signal bars, padlock) are inline SVG that inherit `currentColor` and carry a
`.visually-hidden` label. No icon font, no sprite, no third icon.

## Tokens

| Group | Tokens |
|---|---|
| Brand | `--c-brand`, `--c-brand-light` |
| Surface | `--c-bg`, `--c-surface`, `--c-raised` |
| Line | `--c-border`, `--c-border-strong` |
| Text | `--c-text`, `--c-text-muted` |
| Accent | `--c-accent`, `--c-accent-hover`, `--c-accent-active`, `--c-on-accent`, `--c-focus` |
| Status | `--c-ok`, `--c-warn`, `--c-danger`, `--c-offline`, `--c-busy`, each with a `-bg`; plus `--c-danger-hover`, `--c-danger-active`, `--c-on-danger` |
| Elevation | `--shadow-1`, `--shadow-2` |
| Type | `--font-sans`, `--font-mono`, `--text-xs…--text-2xl`, `--leading-tight/data/normal`, `--weight-normal/medium/bold`, `--tracking-tight/wide`, `--measure` |
| Space | `--space-1,2,3,4,6,8,12` |
| Shape | `--radius-sm/md/lg/pill`, `--border`, `--border-strong`, `--rail-width` |
| Layout | `--control-h`, `--control-h-sm`, `--page-max` |
| Motion | `--dur-fast/base/slow`, `--ease-out`, `--ease-in-out`, `--press-scale` |

Classes: `.btn` with `--primary/--secondary/--danger/--ghost/--sm`; `.panel`, `.panel--rail`;
`.badge`, `.dot`; state classes `.is-ok/.is-warn/.is-error/.is-offline/.is-busy` which set
`--status`, `--status-bg` and `--rail` together; `.muted`, `.mono`, `.visually-hidden`;
shell classes `.appbar`, `.mark`, `.page`, `.data-list`.

## Contrast

Every pair below was computed with the WCAG 2.x relative-luminance formula. Text pairs are held to
4.5:1 (AA normal text — no exemption is claimed for large text anywhere), non-text UI parts to
3:1 per 1.4.11. Worst case in each theme is listed; nothing falls below.

| Pair | Light | Dark |
|---|---|---|
| `--c-text` on `--c-bg` | 16.20 | 16.83 |
| `--c-text` on `--c-surface` | 18.25 | 15.23 |
| `--c-text-muted` on `--c-bg` | 5.46 | 8.39 |
| `--c-accent` (link) on `--c-bg` | 4.81 | 8.85 |
| `--c-on-accent` on `--c-accent` (primary button) | 5.42 | 8.53 |
| `--c-on-accent` on `--c-accent-active` | 9.77 | 6.54 |
| `--c-on-danger` on `--c-danger` (danger button) | 5.75 | 6.61 |
| `--c-ok` on `--c-ok-bg` (badge) | 4.58 | 7.11 |
| `--c-warn` on `--c-warn-bg` | 5.38 | 7.60 |
| `--c-danger` on `--c-danger-bg` | 4.91 | 5.78 |
| `--c-offline` on `--c-offline-bg` | 5.16 | 5.35 |
| `--c-busy` on `--c-busy-bg` | 4.60 | 7.24 |
| `--c-border-strong` on `--c-surface` (UI, 3:1) | 3.82 | 3.78 |
| `--c-border-strong` on `--c-bg` (UI, 3:1) | 3.39 | 4.18 |
| `--c-focus` ring on `--c-bg` (UI, 3:1) | 4.81 | 8.85 |

`--c-border` is intentionally below 3:1 in both themes; see the two-border-tokens decision above.
`--c-brand` / `--c-brand-light` are logotype colours used only in the decorative `.mark`, which
1.4.11 exempts.

Re-run the check after any palette edit — the ratios above are the contract, not a snapshot.

## Budget

CSS is 3.62 KB gzipped of the 100 KB total budget; the whole built UI is 19.85 KB gzipped
(HTML 0.49 + CSS 3.62 + JS 15.74), 19 % of the budget. Re-run `tools/check-bundle-size.sh` after
any change.
