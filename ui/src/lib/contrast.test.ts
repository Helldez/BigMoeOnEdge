import css from '../app.css?raw';
import { describe, expect, it } from 'vitest';
import { AA_LARGE, AA_TEXT, contrastRatio, readTokens } from './contrast';


const themes = { light: readTokens(css, 'light'), dark: readTokens(css, 'dark') };

// Every pair the UI draws words on: [foreground, background].
const TEXT: [string, string][] = [
  ...['bg', 'surface', 'surface-2', 'hover', 'code-bg', 'accent-tint'].map((b): [string, string] => ['text', b]),
  ...['bg', 'surface', 'surface-2', 'hover', 'code-bg'].map((b): [string, string] => ['muted', b]),
  ...['bg', 'surface', 'surface-2', 'hover', 'code-bg', 'accent-tint'].map((b): [string, string] => ['accent-ink', b]),
  ['on-accent', 'accent-fill'],
  ['on-accent', 'accent-fill-hover'],
  ['surface', 'err'],
  ...(['ok', 'warn', 'err'] as const).flatMap((c): [string, string][] => [
    [c, `${c}-tint`],
    [c, 'surface'],
    [c, 'bg'],
    [c, 'code-bg'],
    ['text', `${c}-tint`],
  ]),
];

// The edges of controls and the marks that carry meaning without words: 3:1.
const UI: [string, string][] = [
  ['line-strong', 'surface'],
  ['line-strong', 'bg'],
  ['focus', 'bg'],
  ['focus', 'surface'],
  ['accent', 'surface-2'],
  ['accent-fill', 'surface'],
  ['accent-fill', 'bg'],
  ...[1, 2, 3, 4, 5, 6].map((i): [string, string] => [`series-${i}`, 'surface']),
];

describe('palette contrast (WCAG AA)', () => {
  for (const [name, t] of Object.entries(themes)) {
    it.each(TEXT)(`${name}: %s on %s reads as body text`, (fg, bg) => {
      expect(t[fg], fg).toBeDefined();
      expect(t[bg], bg).toBeDefined();
      expect(contrastRatio(t[fg], t[bg])).toBeGreaterThanOrEqual(AA_TEXT);
    });
    it.each(UI)(`${name}: %s on %s is a visible edge`, (fg, bg) => {
      expect(contrastRatio(t[fg], t[bg])).toBeGreaterThanOrEqual(AA_LARGE);
    });
  }

  it('the dark tokens for the system and for the person agree', () => {
    expect(readTokens(css, 'dark-system')).toEqual(themes.dark);
  });

  it('both themes define the same tokens', () => {
    expect(Object.keys(themes.dark).sort()).toEqual(Object.keys(themes.light).sort());
  });
});

describe('contrastRatio', () => {
  it('is 21 for black on white and 1 for a colour on itself', () => {
    expect(contrastRatio('#000000', '#ffffff')).toBeCloseTo(21, 5);
    expect(contrastRatio('#0891b2', '#0891b2')).toBe(1);
  });
  it('accepts the short form', () => {
    expect(contrastRatio('#fff', '#ffffff')).toBe(1);
  });
});
