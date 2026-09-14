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

Three views behind a hash router: the setup wizard, Settings and About. The nav is built from the
`NAV` array in `src/router.ts`; M3 added Dashboard, gear detail and bus tools as three more
entries and one parameterised route.

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

## Screens (M3) — the bus

Dashboard, gear detail and bus tools (SPEC §10 views 2-4). No new colour, type, spacing or motion
token; one new structural device, reused four times; everything else composes what M1 already had.

**The bus speaks its own notation, so the UI does too.** `A3` is a short address, `G0` a group,
`S7` a scene, `0x8A` a status byte. This is what is printed on every DALI commissioning tool and in
Part 102 itself, and it is shorter, unambiguous and already familiar to the person on the ladder.
It is the one place the interface uses jargon, and it earns it. Everything else is plain: "Make it
blink", "Level after power-on", "Nothing answered here on the last scan".

**A healthy fitting has no status colour and no badge.** The M1 rule says status lives on the
leading edge, never in the fill; M3 adds the corollary that most of the time it says nothing at all.
`.gear` leaves `--rail` at its neutral default and renders no badge unless `gearTrouble()` returns
something, so a bus of 64 working fittings is a calm grid and the one with a failed lamp is the
only coloured thing on the screen. A green "OK" badge on every card is the same confetti as a
tinted card, one step removed.

**Absent is three signals, none of them colour.** A `present: false` address keeps its name and its
last values (SPEC §7.4), so it cannot simply be hidden. `.gear--absent` drops the card's fill to
`--c-bg` — it sinks into the page instead of floating on the surface, literally less present than
its neighbours — breaks the edge rail into a dashed one, and disables every control. The badge says
`Absent` and a line under the card says the values are the last ones the gateway saw.

**The segmented control is the nav marker, reused.** `.seg__btn.is-current` carries the same 3 px
bottom edge as `.nav__link.is-current`. It is the only new structural device in M3 and it does four
jobs: a fitting's On/Off, the dashboard's All gear / By group, scan depth, and commissioning mode.
Nothing else was invented to mean "one of these".

**The level readout is the one loud thing on a card, and it is not monospaced.** `.level__value` is
`--text-lg` with `tabular-nums` in the sans face. The mono face is structural everywhere else, but
this is the one readout that mixes digits with a word, and `Off` set in Ubuntu Mono reads as `0ff`
at arm's length. Tabular figures keep the column steady without it.

**Groups are a mode, not a second copy.** Membership lives in each fitting's `config.groups`, which
only the full entry carries, so switching to By group reads the present addresses once, four at a
time, and says so while it does. A fitting in two groups renders in both sections; both cards are
views of one store entry and move together.

**Scenes are a tri-state, and the UI never collapses it.** An unprogrammed scene is `null` (0xFF),
which is not level 0: the fitting ignores the scene call rather than switching off. A programmed
row is a number input plus Clear; an unprogrammed row says so in words and offers "Set a level".
Rows that differ from the fitting carry the edge marker as an inset shadow, the same one the
selected network row uses.

**Writes to a fitting are staged, counted and named.** `.writebar` is the settings save bar scoped
to one panel: it appears only when something differs, counts the changes, and says "Write to
fitting" because that is what the button does — one DTR store per parameter, read back and
verified.

**Re-addressing asks for the address, not for "yes".** `TypeToConfirm` takes the target address as
the word, so the confirming gesture is the same value the user is about to commit. A target that
another present fitting already answers on is blocked before the request, with that fitting named,
rather than left to come back as `address_in_use`. Commissioning `all` is gated the same way, on
the word `READDRESS`, and the question names what breaks: wall switches, controllers and Home
Assistant entities that address fittings by number.

**The raw console pairs a frame with its answer.** One `<li>` per exchange, `TX` above and `RX`,
`ERR` or `··` below, newest exchange first. Interleaving them as independent lines put every reply
above the frame that caused it. `TX`/`RX` rather than an arrow glyph: it is what the protocol calls
them, and it survives at `--text-xs`.

**The M5 monitor had a place, not a placeholder.** The fourth panel on Bus tools rendered in the
offline state with a badge saying `M5` and one sentence about what it would be. M5 replaced it in
place; see below.

### The slider write path

`src/level.ts`. Optimistic locally, and on the wire a queue of exactly one:

1. every `input` event moves the slider and touches nothing else;
2. the value replaces whatever was waiting — only the newest is ever sent (coalescing);
3. it leaves after 150 ms of quiet, or 150 ms after the first unsent value, whichever comes first,
   or immediately on `change` when the user lets go;
4. no two requests start less than 150 ms apart, and none starts while one is in flight.

Rule 3 is a debounce with a ceiling rather than a plain trailing debounce: a finger that keeps
moving never leaves 150 ms of quiet, and a dimmer that shows nothing until you let go is useless in
the room you are standing in. Rules 2 and 4 are the safety: a dragged slider contributes at most one
command to the 32-deep queue at any instant and at most ~7 transactions a second, whatever the
gesture. Measured against the stub, 100 input events over two seconds produce 13 requests, each
carrying the newest value; 51 events in one tick produce one.

The pipe owns the slider position while anything is queued, in flight, or within 900 ms of the last
successful write, so an SSE `gear` event that is still carrying the old level cannot yank the
control out from under the finger. After that the device's value wins again. A refused write shows
the closed error code in the card's own words (`Bus busy`, `Bus unpowered`, `No reply`) and hands
the slider straight back.

### The event stream

`src/events.ts` reads `GET /api/events` with `fetch` and parses SSE by hand instead of using
`EventSource`. The reason is the three-client cap: `EventSource` reports every failure as one
opaque `error` event with no status and retries on a schedule of its own, which turns "someone left
a tab open on the other phone" into a reconnect storm that keeps the user locked out of their own
gateway. Reading the response by hand gives the status code, so:

- **503** is `refused`: a 20 s wait and a banner that says the gateway keeps three connections, it
  has three, and closing one of them is the fix. There is a Try now button for when the user has
  just done that.
- **anything else** is `retrying`: 1 s → 15 s jittered backoff and a banner that says the numbers
  on screen are the last ones the gateway sent.

One stream per page, opened once by `App` and never re-opened by a route change — the gateway
counts listeners, not tabs. A `pagehide` handler aborts it so the slot is freed the moment the page
goes away. A watchdog treats 45 s of silence (three missed 15 s heartbeats) as a dead stream and
reconnects, because a Wi-Fi association that drops without a FIN leaves the reader blocked for
ever.

## Screens (M5) — the passive monitor

The fourth panel on Bus tools, replacing the M5 placeholder. No new colour, type, spacing or motion
token, and no new structural device: it is the raw console's list shell with a different row grid.

**It is a panel on an existing page, not a screen.** The monitor is a diagnostic someone opens
once, usually because a wall switch is doing something unexpected. It sits below the raw console
(the other thing on this page that speaks in frames), it is inert until started, and its list is
capped at `60vh` so it never pushes Scan and Commissioning off the top of a phone.

**Newest first, and the scroll box never moves.** A monitor that appends downwards has to choose
between following the tail — which fights a finger that is trying to read a row — and leaving the
live edge off-screen. Newest-first needs neither: the newest frame is always at a fixed position,
the list can be bounded with `overflow-y: auto` without a single programmatic scroll, and it is the
order the raw console above already uses. The export inverts it to oldest-first, because a log
pasted into a bug report is read the way time runs, and its header says which way round it is.

**The buffer is 200 frames and the header says so.** A burst on a DALI bus outruns a reader by two
orders of magnitude, and the gateway itself drops frames rather than stall its receiver, so a
bigger buffer would buy completeness the protocol path cannot deliver anyway. The count line
reports both numbers — `1 077 frames, newest 200 kept` — so the buffer's edge is never implied by
a list that simply stops.

**Lossiness is stated in the UI, not only in the docs.** A line under the list says the gateway
drops frames under load and so does this page, and that the list is a sample of the bus rather than
a complete capture. The same two sentences head the exported text, because that is where the claim
gets quoted.

**"Your own frames are not here" is said three times, in the three places it is misread.** In the
lede, so it is read before the first frame arrives; in the empty state, which is what a user sees
when they press a dashboard button and expect the monitor to move; and as a `busy` notice while the
gateway is running a scan or a commissioning pass, which is the one moment the panel can sit
motionless for minutes while the bus is saturated.

**The state shown is the device's, never the button's.** `listening` is only ever set from the
`{"ok":true,"listening":bool}` the device answers with, so a refused start leaves the badge off and
puts the device's own words in an error notice. An arriving `rx` frame also sets it: another tab,
or a reload, can leave the monitor running with nothing here having clicked it. The panel also says
that listening is not a setting and does not survive a reboot, because nothing else in this UI
behaves that way.

**Pause freezes the list, not the stream.** Frames that arrive while paused are counted and
dropped rather than queued: at 200 frames of buffer, releasing a queued burst would evict exactly
the rows the user paused to read. The count line then carries `731 skipped while paused` for as
long as the capture lives, so the hole in the buffer is visible in the UI and in the export.

**The bit count is the tag, the way `TX`/`RX` is in the console.** 8, 16 and 24 are what the event
carries and they are what separates a reply from a command from a Part 103 message. Colour repeats
the distinction — accent for a forward frame, green for a backward one, amber for anything
unreadable — and never carries it alone.

**A frame that cannot be read says so.** `readFrame()` in `src/dali.ts` decodes what IEC 62386-102
fixes: the address byte (short address, group, broadcast, the special-command range) and the
opcode, with the scene and group ranges computed rather than tabulated. Anything else — a 24-bit
input-device message, a manufacturer opcode, a reserved address byte — is rendered muted with a
reading that names the gap ("Input device, Part 103 — not decoded"). The hex is always there to
read. A monitor that invents a reading is worse than one that admits it has none. Backward frames
are deliberately *not* correlated with the forward frame above them: it is usually the answer to
it, and "usually" is not good enough when the path in front of it drops frames.

**The list is deliberately not a live region.** A burst would read hundreds of rows aloud and bury
the controls. The counts change at the same time and are the thing worth hearing, and the notices
already carry `role="status"` / `role="alert"`.

**Timestamps are the device's uptime, not a clock.** `ts` is milliseconds since boot — the gateway
has no wall clock in AP mode — so it is rendered `H:MM:SS.mmm` in the mono face, which lines a
captured frame up with a line in the device log.

### Where the frames live

`src/monitor.ts`, not `src/store.ts`. A store commit re-renders the dashboard, the gear grid and
the bus banner, and foreign traffic arrives two orders of magnitude faster than a gear update.
Frames land in a pending array and are published on a 120 ms timer, so a 250 frame/s burst costs
about eight renders a second in one panel instead of 250 everywhere. The reading is computed once
on arrival rather than on every render, which keeps the row a plain span. Measured against the
stub: 900 frames in ~4 s hold the list at 200 rows with no dropped input and no scroll jump.

Clipboard and download both exist because the device is served over plain HTTP on the LAN, where
browsers leave `navigator.clipboard` undefined. A refusal is the normal case, not the edge one, so
it is caught and answered with a notice pointing at Save as text.

## Additions to the vocabulary

No new colour, type, spacing or motion token. New classes only, all composed from existing tokens.

### M1

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

### M3

| Class | Role |
|---|---|
| `.banner` | the event-stream notice, sticky under the nav |
| `.seg`, `.seg__btn`, `.seg__btn.is-current` | exclusive choices, marked with the nav's edge |
| `.level`, `.level__range`, `.level__value` | the dimmer and its readout |
| `.toolbar` | a panel that is only controls |
| `.gears` | the responsive card grid, one column at phone width |
| `.gear`, `.gear__head`, `.gear__name`, `.gear__addr`, `.gear__foot`, `.gear__note`, `.gear__error`, `.gear__data`, `.gear--absent` | the fitting card |
| `.group`, `.group__summary`, `.group__id`, `.group__name`, `.group__count`, `.group__body`, `.group__controls` | a collapsible group section |
| `.scenes`, `.scene`, `.scene.is-changed`, `.scene__id`, `.scene__empty`, `.scene__level` | the 16-row scene table |
| `.groups`, `.groupbox`, `.groupbox.is-on` | the group membership checkboxes |
| `.writebar` | unwritten fitting parameters |
| `.table-scroll`, `.table` | the commissioning result table |
| `.console`, `.console__row`, `.console__line`, `.console__dir`, `.console__dir--tx/--rx/--err/--wait`, `.console__text` | the raw frame history |
| `.backlink`, `.tools__actions`, `.query__form`, `.query__reply` | small layout helpers |

### M5

| Class | Role |
|---|---|
| `.monitor` | the console shell, bounded to `60vh` and scrollable |
| `.monitor__row`, `.monitor__time`, `.monitor__hex`, `.monitor__bits`, `.monitor__bits--forward/--backward/--unknown`, `.monitor__text` | one received frame: uptime, hex, bit-count tag, reading |
| `.monitor__bar`, `.monitor__count`, `.monitor__note` | the list's toolbar, its counts and the caveats under it |

`.monitor__row` overrides `.console__row`'s flex column at the same specificity, so it has to stay
after it in the file.

The dimmer is a bare `<input type="range">` styled only by `accent-color`, which the token layer
already sets. The browser draws a filled track and a thumb that follow the viewer's colour scheme
and platform sizing, and the 44 px input box is the drag target whatever size the thumb renders at.
Re-cutting all four vendor pseudo-elements would buy a slightly different grey and four more ways
to break.

`.btn--sm` exists in the token layer but is not used on any touch target in M1: every button and
input in these screens is at least `--control-h` (44 px).

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

M3 added `--c-raised` as a text-bearing surface (the console, the write bar) and `--c-bg` as the
fill of an absent card, so those pairs are held to the same contract:

| Pair | Light | Dark |
|---|---|---|
| `--c-text` on `--c-raised` | 17.28 | 13.71 |
| `--c-text-muted` on `--c-raised` | 5.82 | 6.84 |
| `--c-accent` on `--c-raised` (console TX) | 5.13 | 7.21 |
| `--c-ok` on `--c-raised` (console RX) | 5.06 | 7.01 |
| `--c-danger` on `--c-raised` (console ERR) | 5.44 | 5.32 |
| `--c-text-muted` on `--c-surface` (segmented, unselected) | 6.15 | 7.59 |
| `--c-accent` on `--c-surface` (segmented, selected) | 5.42 | 8.00 |
| `--c-offline` on `--c-bg` (absent card) | 5.46 | 6.64 |
| `--c-border-strong` on `--c-raised` (UI, 3:1) | 3.62 | 3.41 |

M5 put `--c-warn` on `--c-raised` (the tag of a frame that cannot be decoded), which is the only
pair it added:

| Pair | Light | Dark |
|---|---|---|
| `--c-warn` on `--c-raised` (monitor, unreadable frame) | 5.82 | 7.38 |

`--c-border` is intentionally below 3:1 in both themes; see the two-border-tokens decision above.
`--c-brand` / `--c-brand-light` are logotype colours used only in the decorative `.mark`, which
1.4.11 exempts.

Re-run the check after any palette edit — the ratios above are the contract, not a snapshot.

## Budget

After M5 the whole built UI is 34.29 KB gzipped (HTML 0.48 + CSS 4.63 + JS 29.18), 34 % of the
100 KB budget; M3 ended at 31.45 KB and the monitor, its Part 102 opcode table included, cost
2.1 KB of it. Still no runtime dependency beyond Preact — no virtual list, no clipboard shim.
Re-run `tools/check-bundle-size.sh` after any change.
