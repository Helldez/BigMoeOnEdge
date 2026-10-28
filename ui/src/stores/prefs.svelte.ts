// The UI's own preferences, kept in this browser: theme, text size, the sidebar folded, the preview
// pane's width. None is an engine parameter.

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

class Prefs {
  theme = $state<Theme>(THEMES.includes(read('bmoe.theme') as Theme) ? (read('bmoe.theme') as Theme) : 'system');
  textScale = $state<number>(((v) => (TEXT_SCALES.includes(v as never) ? v : 1))(Number(read('bmoe.textScale'))));
  sidebarCollapsed = $state(read('bmoe.sidebar') === 'collapsed');
  previewWidth = $state<number>(((v) => (v >= PREVIEW_MIN && v <= PREVIEW_MAX ? v : 520))(Number(read('bmoe.previewWidth'))));

  setTheme(v: Theme): void {
    this.theme = v;
    write('bmoe.theme', v);
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

  /** Puts the choices on <html>: the theme as `data-theme` (absent = the system's), the text size. */
  apply(): void {
    if (typeof document === 'undefined') return;
    const root = document.documentElement;
    if (this.theme === 'system') delete root.dataset.theme;
    else root.dataset.theme = this.theme;
    root.style.setProperty('--text-scale', String(this.textScale));
  }
}

export const prefs = new Prefs();

