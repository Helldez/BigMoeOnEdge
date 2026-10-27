// The UI's own preferences, kept in this browser: theme, language, text size, the sidebar folded,
// the preview pane's width. None is an engine parameter. `t` and `tp` read the language from here,
// so every word on screen follows a change at once.
import { LANGUAGES, pickLanguage, plural, translate, type Lang } from '../lib/i18n';

export type Theme = 'system' | 'light' | 'dark';
export const THEMES: readonly Theme[] = ['system', 'light', 'dark'];
/** The text sizes offered, as a scale of the 15 px base (docs/design.md, "Type"). */
export const TEXT_SCALES = [0.9, 1, 1.1, 1.25] as const;
export const PREVIEW_MIN = 320;
export const PREVIEW_MAX = 1100;

function read(key: string): string | null {
  try {
    return localStorage.getItem(key);
  } catch {
    return null;
  }
}
function write(key: string, value: string): void {
  try {
    localStorage.setItem(key, value);
  } catch {
    // remembered for this page only
  }
}

function browserLanguages(): string[] {
  if (typeof navigator === 'undefined') return [];
  return navigator.languages?.length ? [...navigator.languages] : navigator.language ? [navigator.language] : [];
}

class Prefs {
  theme = $state<Theme>(THEMES.includes(read('bmoe.theme') as Theme) ? (read('bmoe.theme') as Theme) : 'system');
  /** 'auto' follows the browser. */
  langChoice = $state<'auto' | Lang>(((v) => (v && v in LANGUAGES ? (v as Lang) : 'auto'))(read('bmoe.lang')));
  textScale = $state<number>(((v) => (TEXT_SCALES.includes(v as never) ? v : 1))(Number(read('bmoe.textScale'))));
  sidebarCollapsed = $state(read('bmoe.sidebar') === 'collapsed');
  previewWidth = $state<number>(((v) => (v >= PREVIEW_MIN && v <= PREVIEW_MAX ? v : 520))(Number(read('bmoe.previewWidth'))));

  lang = $derived<Lang>(this.langChoice === 'auto' ? pickLanguage(browserLanguages()) : this.langChoice);

  setTheme(v: Theme): void {
    this.theme = v;
    write('bmoe.theme', v);
    this.apply();
  }
  setLang(v: 'auto' | Lang): void {
    this.langChoice = v;
    write('bmoe.lang', v);
    this.apply();
  }
  setTextScale(v: number): void {
    this.textScale = v;
    write('bmoe.textScale', String(v));
    this.apply();
  }
  setSidebarCollapsed(v: boolean): void {
    this.sidebarCollapsed = v;
    write('bmoe.sidebar', v ? 'collapsed' : 'open');
  }
  setPreviewWidth(v: number): void {
    this.previewWidth = Math.round(Math.min(PREVIEW_MAX, Math.max(PREVIEW_MIN, v)));
    write('bmoe.previewWidth', String(this.previewWidth));
  }

  /** Puts the choices on <html>: the theme as `data-theme` (absent = the system's), the language, the text size. */
  apply(): void {
    if (typeof document === 'undefined') return;
    const root = document.documentElement;
    if (this.theme === 'system') delete root.dataset.theme;
    else root.dataset.theme = this.theme;
    root.lang = this.lang;
    root.style.setProperty('--text-scale', String(this.textScale));
  }
}

export const prefs = new Prefs();

/** A word in the language in force. */
export const t = (key: string, vars?: Record<string, string | number>): string => translate(prefs.lang, key, vars);
/** A count with its plural form (`<key>.one` / `<key>.other`). */
export const tp = (key: string, n: number, vars?: Record<string, string | number>): string => plural(prefs.lang, key, n, vars);
