# The visual language

One look for every UI of this project, native in each toolkit. The five screens say what each UI
shows ([screens.md](screens.md)); this page says how it looks. The style is pixel: the project's
logo is a 19 by 19 pixel chip with cyan eyes (`docs/assets/logo/icon.svg`), and everything else
follows it, so a person who sees the icon on a phone and the page on a laptop sees one product.

## The palette

Taken from the logo. Dark is the default (the engine's home is a phone at night and a terminal);
light is the same palette turned inside out, for a desktop in daylight or a system that asks.

| Token | Dark | Light | What it is for |
|---|---|---|---|
| `bg` | `#0A0512` | `#F2ECFF` | the page |
| `surface` | `#160B2A` | `#FFFFFF` | cards, inputs, the bars |
| `surface-2` | `#2A1150` | `#E6DCFF` | a selected row, a secondary button |
| `border` | `#3B2A63` | `#6C5AA8` | every edge, 2 px |
| `text` | `#F2ECFF` | `#0A0512` | words |
| `muted` | `#B9A9E6` | `#4B3A7A` | secondary words, labels |
| `accent` | `#21E6FF` | `#0FA8C2` | the one action on a screen, links, the picked choice |
| `accent-text` | `#0A0512` | `#FFFFFF` | words on the accent |
| `accent-2` | `#6C5AA8` | `#6C5AA8` | the model's side of a conversation, section titles |
| `warn` | `#FFC53D` | `#8A5A00` | a lever that changes the reply, a stop at the limit |
| `ok` | `#7CE0B0` | `#1D7A46` | ready, on disk, a measured value |
| `err` | `#FF6B8B` | `#B42318` | an error |
| `shadow` | `#000000` | `#0A0512` | the hard shadow |

Semantic colours get a `-bg` tint (the colour at 18% over the surface) for banners and badges.

## Type

- **Pixelify Sans** (SIL OFL 1.1, bundled: `ui/public/fonts`, `res/font`) for what the product
  says in its own voice: titles, the five screen names, section headings, badges, and every figure
  the engine reports (a rate, a count, a percentage). Weight 400 to 700; never below 13 px.
- **The system sans** for prose: the model's answers, the person's messages, help text. Pixel
  text at paragraph length is a strain, and the model's words are not the product's voice.
- **The system mono** for anything a person may paste: a command line, a path, a key.

## Geometry

- An 8 px grid: spacing is 8, 16, 24, 32.
- **No rounded corners.** Every box is square: cards, inputs, buttons, badges, meters.
- **Edges are 2 px**, in `border`; the focused control's edge is `accent`.
- **A hard shadow**, 4 px right and 4 px down in `shadow`, with no blur, on cards and primary
  buttons; a pressed button moves 2 px into its shadow. Secondary buttons have the edge and no
  shadow. Nothing is translucent, nothing glows.
- Meters and progress bars are flat blocks: `surface-2` track, `accent` fill, a 2 px edge.
- Icons are drawn on the pixel grid where a UI draws its own (the logo, the meters); a toolkit's
  stock icons are acceptable in the navigation until pixel ones replace them.

## The logo

The chip is the app icon, the tab icon and the header mark: 24 px beside the wordmark
"BigMoeOnEdge" in Pixelify Sans 700 on the desktop, 24 dp in the phone's top bar. It is never
recoloured, stretched or placed on a busy background; on light surfaces it keeps its own dark
outline. The launcher icon is the chip on `bg`.

## What each toolkit does with this

- **Web**: the tokens are CSS custom properties on `:root`, dark by default and light under
  `prefers-color-scheme: light`; the font is a `@font-face`; the geometry is the stylesheet's
  rules for `button`, `input`, `.card`, `.badge`, `meter`. No component library.
- **Android**: a Material 3 colour scheme built from the same hex values, a `Typography` with
  Pixelify Sans for the title and label styles, and `Shapes` with every corner at 0 dp; the top
  bar carries the mark. Material's ripple and elevation stay, because a phone's touch feedback is
  what the toolkit is for.
- **iOS**, when it comes: the same hex values as a colour set, the same font, `cornerRadius` 0.

What is not in the language: gradients, blur, rounded avatars, animated backgrounds, and any
colour that is not in the table. A screen that needs a sixth colour has a layout problem.
