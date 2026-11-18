# The visual language: Letterhead

One look for every UI of this project, native in each toolkit. [screens.md](screens.md) says what
each UI shows; this page says how it looks. The look is a private studio's stationery: a deep ink
shell, white sheets lifted off a pale ground, one sober sans-serif, and the project's logo, a 19 by
19 pixel chip with cyan eyes (`docs/assets/logo/icon.svg`), as the mark beside the engine's state.
It is written for ordinary people: the engine's figures are one tap away, never the default view.

## The palette: ink and paper

Light and dark are both first-class. A UI follows the system's appearance and lets the person pick
one in Settings (Appearance). Every value below is a token: no UI writes a colour that is not in
this table.

| Token | Light | Dark | What it is for |
|---|---|---|---|
| `bg` | `#F3F4F8` | `#10121F` | the desk: the page behind the sheets |
| `surface` | `#FFFFFF` | `#191C30` | a sheet: an answer, a card, the composer, a dialog |
| `surface-2` | `#ECEEF4` | `#20243C` | a quiet fill: a track, an inner box |
| `hover` | `#E6E8F1` | `#272B47` | under the pointer |
| `line` | `#DCDFEA` | `#2C3150` | dividers, 1 px |
| `line-strong` | `#6E7396` | `#7D82A8` | the edge of a field or a switch's track: a control's outline must be seen (3:1) |
| `text` | `#1B1F3A` | `#E8E9F4` | words |
| `muted` | `#555A7C` | `#A9ADC9` | secondary words, labels, help, the engine's state in words |
| `accent` | `#2F3A8F` | `#8E9BFF` | marks: a meter's fill, a selected edge |
| `accent-ink` | `#2F3A8F` | `#AAB4FF` | the accent as words: links |
| `accent-fill` | `#1B1F3A` | `#E8E9F4` | the one primary button of a view (Send, Apply plan): ink |
| `accent-fill-hover` | `#2C3160` | `#FFFFFF` | the primary button under the pointer |
| `accent-edge` | `#1B1F3A` | `#E8E9F4` | the primary button's edge |
| `on-accent` | `#FFFFFF` | `#10121F` | words on `accent-fill` |
| `accent-tint` | `#E6E8F4` | `#23284A` | an information banner, the selected item in a list |
| `focus` | `#2F3A8F` | `#8E9BFF` | the keyboard focus ring, 2 px, 2 px off the control |
| `shell` | `#1B1F3A` | `#0B0D18` | the sidebar and, on a phone, the top bar |
| `shell-hi` | `#2C3160` | `#23284A` | the open conversation in the shell; the person's message |
| `on-shell` | `#FFFFFF` | `#FFFFFF` | words on the shell |
| `on-shell-muted` | `#C7CAE0` | `#B7BAD4` | secondary words on the shell |
| `seal` | `#FFC53D` | `#FFC53D` | the logo's yellow, only as the rule under the logo on the welcome sheet |
| `ok` | `#157A3C` | `#4ADE80` | ready, done, on disk, a measured value |
| `ok-tint` | `#E6F4EA` | `#12261B` | behind `ok` words: a badge, a banner |
| `warn` | `#9A4A00` | `#FBBF24` | waiting, a lever that changes the reply, a stop at a limit |
| `warn-tint` | `#FFF1D6` | `#2A2111` | behind `warn` words |
| `err` | `#B42318` | `#F87171` | an error, a destructive action |
| `err-tint` | `#FDECEA` | `#2C1517` | behind `err` words |
| `code-bg` | `#F3F4F8` | `#141729` | code blocks, inline code, a diff |
| `series-1` … `series-6` | `#2F3A8F` `#0B8EA8` `#B87400` `#2E8B57` `#C2416E` `#6B6F88` | `#8E9BFF` `#21E6FF` `#FFC53D` `#4ADE80` `#FB7185` `#A7ABC4` | the parts of one whole, told apart by hue: the plan's memory pools, part *i* taking `series-(i mod 6 + 1)`; each one 3:1 on `surface` |

Shadows lift sheets instead of frames: `shadow-sm` (`0 1px 2px` at 8% of `text`), `shadow-md` (a
sheet: `shadow-sm` plus `0 8px 24px` at 8%), `shadow-lg` (a dialog). In dark they are black at
35-55%. A dialog's backdrop is `text` at 45% in light, black at 60% in dark.

## Contrast

Every pair of a word and what it sits on meets WCAG AA: **4.5:1** for body text, **3:1** for
large text (18.5 px bold or 24 px), the edge of a control and a mark that carries meaning on its
own. The web UI's test (`ui/src/lib/contrast.test.ts`) reads the tokens out of the stylesheet and
fails the build when a pair it lists drops below its bar, in both themes, the shell's pairs
included; a new pair a screen draws is added to that list in the same change. Colour never says
something alone: a state has its words (and a dot or an icon beside them), a diff has its `+` and `−`.

## Type

- **Hanken Grotesk** (SIL OFL 1.1, by The Hanken Grotesk Project Authors; bundled: the web UI's
  `ui/public/fonts/HankenGrotesk-latin*.woff2`, its licence beside them) for every word: the
  wordmark, titles, labels, buttons, messages. Weights 400 (body), 550 (buttons, badges), 600 to
  650 (titles).
- **JetBrains Mono** (SIL OFL 1.1, by The JetBrains Mono Project Authors; bundled as a variable
  woff2) for anything a person may paste or a machine wrote: code, a command, a path, a key, a diff.
- Sizes on a 15 px base: 24 (page title), 18 (section), 15 (body), 13 (help, captions, small
  buttons), 12 (badges). Line height 1.5 for reading, 1.25 for titles.
- **Tabular digits** for every figure the engine reports (a rate, a share, a size), so a value that
  changes does not shift the words around it.
- The person can set the text size (Settings, Appearance: 90 to 125%); every size above is
  relative to it.

## Geometry

- A **4/8 px grid**: spacing is 4, 8, 12, 16, 24, 32.
- **Corners**: 6 px on sheets (answers, cards, the preview), 10 px on controls (fields, buttons,
  the composer, menus); **pills** for badges, switches and meters. The person's message is a
  10 px bubble with a 3 px corner at its foot on the right.
- **Sheets are lifted, not framed**: `surface` on `bg` with `shadow-md`; a 1 px `line` only where
  rows inside a sheet need parting. A field's edge is `line-strong`; the focused control has a
  2 px `focus` ring 2 px outside it, always visible from the keyboard.
- **Controls**: 36 px tall (28 px small); a phone layout keeps every touch target at 44 px or more.
- Progress bars are 8 px pills: `surface-2` track, `accent` fill. A share the engine measures on the
  Plan (found in memory, memory held, a device's rate) is a cell meter: see below.
- Icons are 16 or 20 px line drawings with a 1.5 px stroke, round caps, drawn for this UI (the web
  UI's are inline SVG in `ui/src/components/Icon.svelte`); a toolkit's own icons stand in on a
  platform that has a standard set.

## The logo and the engine's state

The chip is the app icon, the tab icon and the mark in the shell: 24 px beside the wordmark
"BigMoeOnEdge" in Hanken Grotesk 650. It is drawn on its pixel grid with crisp edges
(`image-rendering: pixelated` where scaled) and is never recoloured, stretched, rounded or placed on
a busy background. Two more places carry it, and only these:

- **The state line**, at the top of a conversation: the chip at 28 to 36 px beside one line of
  plain words in `muted` saying what the engine is doing (Thinking…, Writing…, Reading part of the
  model from the disk…, Answered, Waiting for your approval below, No model loaded). While the
  engine works the chip fades between full and 45% over 1.4 s; nothing else in the app moves on
  its own. Web: `ui/src/components/ChipBuddy.svelte`.
- **The welcome**: on an empty conversation the chip at 64 to 96 px on a white sheet, with the
  `seal` yellow as a 3 px rule under it, over "Private: everything stays on this computer."

## The engine's figures

One quiet line under each finished answer, after its actions: `tg 12.4 tok/s · pp 140 tok/s`
(the writing speed and the prompt-reading speed), in `muted`, tabular digits. It opens, as a
disclosure, to every figure of that answer with a line saying what it measures, the speed against
what the plan expected, and the way to the Plan and to Metrics. While the answer streams the same
place shows the live `tg`. Nothing else in a conversation shows a figure.

## The cell meter

On the Plan and in Metrics a share the engine measures is counted in square cells, the logo's
pixel grid: 20 cells in a row, 2 px apart, 1 px corners, `accent` for the cells filled, `surface-2`
for the rest; a cell for a device the run does not use fills in `line-strong`. A reference value
(what the plan credited) outlines its cell, 2 px in `text`. The figure is always written beside the
cells. The plan's memory pools are cells too, 40 to a row, each part a run of cells in its series
colour and the free part in `surface-2`; every part held keeps at least one cell. Web:
`ui/src/components/CellMeter.svelte` (`role="meter"`, classes only) and `memoryCells` in
`ui/src/lib/plan.ts`.

## Disclosures

Whatever opens shows a chevron after its words that turns when it is open; the browser's own
triangle is never shown. A row that lays out its own summary draws the same chevron at its end.

## The preview

A file the agent made lies on the desk beside the conversation: the pane is `bg`, its name and
actions on top, the file itself on a `surface` sheet with `shadow-md`; full screen, the sheet takes
the whole window without margin or shadow.

## Motion

Short and functional: 120 ms for a hover or a toggle, 200 ms for a pane that opens or a bar that
fills, eased out. Nothing moves on its own except what reports progress (the chip while the engine
works, a skeleton's shimmer, a running tool's indicator). With the system's **reduced motion**
setting every transition and animation is cut to nothing.

## What each toolkit does with this

- **Web**: the tokens are CSS custom properties on `:root` (`ui/src/app.css`), light by default,
  dark under `prefers-color-scheme: dark` unless the person picked light, and dark whenever they
  picked it (`data-theme` on `<html>`); the shell redefines the shared tokens for what sits on it.
  The fonts are `@font-face` rules over the bundled files. No component library, no style attribute
  (the server's Content-Security-Policy allows none).
- **Android**: a Material 3 colour scheme from the same hex values (light and dark), a
  `Typography` with Hanken Grotesk, the shell colour on the top bar and the drawer. Material's
  ripple and elevation stay: a phone's touch feedback is what the toolkit is for.
- **iOS**: the same hex values as colours that follow the system's appearance, Hanken Grotesk
  registered from the app bundle, the logo drawn from `docs/assets/logo/icon.svg` itself
  (`examples/ios/app/Sources/Design`). The system's bars and sheets stay, tinted with the shell.

What is not in the language: gradients (but a skeleton's shimmer), blur, glows, hard offset
shadows, decorative animation, mascots that talk, and any colour that is not in the table. A screen
that needs another colour has a layout problem.
