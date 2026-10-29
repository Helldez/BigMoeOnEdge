// WCAG 2 contrast, for the palette check (docs/design.md, "Contrast"). Pure: the test feeds it the
// tokens it reads out of app.css.

/** A `#rrggbb` (or `#rgb`) colour's relative luminance, per WCAG 2.x. */
export function luminance(hex: string): number {
  let h = hex.trim().replace(/^#/, '');
  if (h.length === 3) h = [...h].map((c) => c + c).join('');
  if (!/^[0-9a-fA-F]{6}$/.test(h)) throw new Error(`not a hex colour: ${hex}`);
  const n = parseInt(h, 16);
  const channel = (v: number) => {
    const c = v / 255;
    return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * channel((n >> 16) & 255) + 0.7152 * channel((n >> 8) & 255) + 0.0722 * channel(n & 255);
}

/** The contrast ratio of two colours, from 1 to 21. */
export function contrastRatio(a: string, b: string): number {
  const x = luminance(a);
  const y = luminance(b);
  return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05);
}

/** The minimum a pair needs: 4.5 for body text, 3 for large text and the edges of controls. */
export const AA_TEXT = 4.5;
export const AA_LARGE = 3;

/**
 * The tokens of one `/* tokens:<name> *\/ ... /* end tokens *\/` block of a stylesheet, as
 * `{name: '#rrggbb'}`; only hex values are kept (shadows and scrims are not colours a word sits on).
 */
export function readTokens(css: string, block: string): Record<string, string> {
  const start = css.indexOf(`/* tokens:${block} */`);
  if (start < 0) throw new Error(`no token block ${block}`);
  const end = css.indexOf('/* end tokens */', start);
  const body = css.slice(start, end < 0 ? undefined : end);
  const out: Record<string, string> = {};
  for (const m of body.matchAll(/--([a-z0-9-]+):\s*(#[0-9a-fA-F]{3,6})\s*;/g)) out[m[1]] = m[2].toLowerCase();
  return out;
}
