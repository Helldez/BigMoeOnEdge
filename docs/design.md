# The visual language

One look for every UI of this project, native in each toolkit. [screens.md](screens.md) says what
each UI shows; this page says how it looks. The look is calm and plain: slate surfaces, one cyan
accent, a readable sans-serif, rounded boxes with thin lines and soft shadows. The project's logo,
a 19 by 19 pixel chip with cyan eyes (`docs/assets/logo/icon.svg`), is the one pixel object, and the
accent is its eyes' colour, so the icon on a phone and the page on a laptop are one product.

## The palette: slate and cyan

Light and dark are both first-class. A UI follows the system's appearance and lets the person pick
one in Settings (Appearance). Every value below is a token: no UI writes a colour that is not in
this table.

| Token | Light | Dark | What it is for |
|---|---|---|---|
| `bg` | `#F6F8F9` | `#0B1014` | the page, behind everything |
| `surface` | `#FFFFFF` | `#131A20` | cards, fields, the sidebar, dialogs |
| `surface-2` | `#EEF2F4` | `#1A232B` | a quiet fill: a selected row, a track, a user's message |
| `hover` | `#E8EDF0` | `#202A33` | under the pointer |
| `line` | `#DFE5E9` | `#222C34` | dividers and card edges, 1 px |
| `line-strong` | `#7D8B96` | `#5B6B78` | the edge of a field or a switch's track: a control's outline must be seen (3:1) |
| `text` | `#111820` | `#E6EDF2` | words |
| `muted` | `#56636E` | `#93A1AC` | secondary words, labels, help |
| `accent` | `#0891B2` | `#22B8D6` | the brand cyan: a meter's fill, a selected edge, the focus ring |
| `accent-ink` | `#0E7490` | `#22B8D6` | the accent as words: links, a selected label |
| `accent-fill` | `#0E7490` | `#22B8D6` | the one primary button of a view, a switch that is on |
| `accent-fill-hover` | `#155E75` | `#4CC9E1` | the primary button under the pointer |
| `on-accent` | `#FFFFFF` | `#0B1014` | words on `accent-fill` |
| `accent-tint` | `#E6F4F8` | `#0F2A33` | an information banner, the selected item in a list |
| `focus` | `#0891B2` | `#22B8D6` | the keyboard focus ring, 2 px, 2 px off the control |
| `ok` | `#157A3C` | `#4ADE80` | ready, done, on disk, a measured value |
| `ok-tint` | `#EAF6EE` | `#12261B` | behind `ok` words: a badge, a banner |
| `warn` | `#A14A07` | `#FBBF24` | waiting, a lever that changes the reply, a stop at a limit |
| `warn-tint` | `#FDF3E2` | `#2A2111` | behind `warn` words |
| `err` | `#B91C1C` | `#F87171` | an error, a destructive action |
| `err-tint` | `#FCEBEB` | `#2C1517` | behind `err` words |
| `code-bg` | `#F1F4F6` | `#0F151A` | code blocks, inline code, a diff |

Shadows are soft and few: `shadow-sm` (cards, buttons; `0 1px 2px` at 6% of `text` in light, 40%
black in dark), `shadow-md` (a popover, a menu), `shadow-lg` (a dialog). A dialog's backdrop is
`text` at 40% in light, black at 55% in dark.

Why the light accent has three tokens: the brand cyan `#0891B2` is 3.7:1 on white, enough for a
ring or a fill (3:1) and not for words (4.5:1). Words and the primary button's fill take the deeper
`#0E7490` (5.4:1 with white); in dark the brand cyan reads everywhere, so the three are one value.

## Contrast

Every pair of a word and what it sits on meets WCAG AA: **4.5:1** for body text, **3:1** for
large text (18.5 px bold or 24 px), the edge of a control and a mark that carries meaning on its
own. The web UI's test (`ui/src/lib/contrast.test.ts`) reads the tokens out of the stylesheet and
fails the build when a pair it lists drops below its bar, in both themes; a new pair a screen draws
is added to that list in the same change. Colour never says something alone: a state has its words
(and a dot or an icon beside them), a diff has its `+` and `−`.

## Type

- **Inter** (SIL OFL 1.1, by The Inter Project Authors; bundled: `ui/public/fonts/InterVariable.woff2`,
  its licence beside it) for every word: the wordmark, titles, labels, buttons, messages. Weights
  400 (body), 550 (buttons, badges), 600 to 650 (titles).
- **JetBrains Mono** (SIL OFL 1.1, by The JetBrains Mono Project Authors; bundled as a variable
  woff2) for anything a person may paste or a machine wrote: code, a command, a path, a key, a diff.
- Sizes on a 15 px base: 24 (page title), 18 (section), 15 (body), 13 (help, captions, small
  buttons), 12 (badges). Line height 1.5 for reading, 1.25 for titles.
- **Tabular digits** for every figure the engine reports live (a rate, a share, a size), so a
  value that changes does not shift the words around it.
- The person can set the text size (Settings, Appearance: 90 to 125%); every size above is
  relative to it.

## Geometry

- A **4/8 px grid**: spacing is 4, 8, 12, 16, 24, 32.
- **Corners**: 8 px on cards, fields, buttons, menus and dialogs; 6 px on small inner boxes;
  **pills** (fully round ends) for badges, switches and meters.
- **Lines are 1 px**, in `line`; a field's edge is `line-strong`; the focused control has a 2 px
  `focus` ring 2 px outside it, always visible from the keyboard.
- **Controls**: 36 px tall (28 px small), so a pointer and a finger both find them; a phone layout
  keeps every touch target at 44 px or more.
- Meters and progress bars are 8 px pills: `surface-2` track, `accent` fill.
- Icons are 16 or 20 px line drawings with a 1.5 px stroke, round caps, drawn for this UI (the web
  UI's are inline SVG in `ui/src/components/Icon.svelte`); a toolkit's own icons stand in on a
  platform that has a standard set.

## Motion

Short and functional: 120 ms for a hover or a toggle, 200 ms for a pane that opens or a bar that
fills, eased out. Nothing moves on its own except what reports progress (a skeleton's shimmer, a
running tool's indicator). With the system's **reduced motion** setting every transition and
animation is cut to nothing.

## The logo

The chip is the app icon, the tab icon and the mark in the sidebar: 24 px beside the wordmark
"BigMoeOnEdge" in Inter 650. It is drawn on its pixel grid with crisp edges (`image-rendering:
pixelated` where scaled) and is never recoloured, stretched, rounded or placed on a busy
background; on light surfaces it keeps its own dark outline. The launcher icon is the chip on `bg`.

## What each toolkit does with this

- **Web**: the tokens are CSS custom properties on `:root` (`ui/src/app.css`), light by default,
  dark under `prefers-color-scheme: dark` unless the person picked light, and dark whenever they
  picked it (`data-theme` on `<html>`). The fonts are `@font-face` rules over the bundled files; the
  geometry is the stylesheet's rules for `button`, `input`, `.card`, `.badge`, `.banner`, `meter`.
  No component library, no style attribute (the server's Content-Security-Policy allows none).
- **Android**: a Material 3 colour scheme from the same hex values (light and dark), a
  `Typography` with Inter, `Shapes` at 8 dp; the top bar carries the mark. Material's ripple and
  elevation stay: a phone's touch feedback is what the toolkit is for.
- **iOS**: the same hex values as colours that follow the system's appearance, Inter registered
  from the app bundle, 8 pt continuous corners, the logo drawn from `docs/assets/logo/icon.svg`
  itself (`examples/ios/app/Sources/Design`). The system's bars and sheets stay, tinted.

What is not in the language: gradients (but a skeleton's shimmer), blur, glows, hard offset
shadows, decorative animation, and any colour that is not in the table. A screen that needs another
colour has a layout problem.
