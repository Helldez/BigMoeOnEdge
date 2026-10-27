// A small dictionary-based translation layer. English is the key language: every key exists in
// `en`, and `i18n.test.ts` fails when another language misses one (or keeps one English dropped).
// Values may name `{placeholders}`; a plural picks `<key>.one` or `<key>.other` by `n`.
import { en } from './en';
import { it } from './it';

export type Dict = Record<string, string>;
export const LANGUAGES = { en, it } as const satisfies Record<string, Dict>;
export type Lang = keyof typeof LANGUAGES;
export const LANG_NAMES: Record<Lang, string> = { en: 'English', it: 'Italiano' };

/** The language a browser asks for, among the ones there are; English otherwise. */
export function pickLanguage(preferred: readonly string[]): Lang {
  for (const p of preferred) {
    const base = p.toLowerCase().split('-')[0];
    if (base in LANGUAGES) return base as Lang;
  }
  return 'en';
}

/** `key` in `lang`, its placeholders filled; the English text when the language lacks it, the key itself when both do. */
export function translate(lang: Lang, key: string, vars?: Record<string, string | number>): string {
  const d: Dict = LANGUAGES[lang];
  let s = d[key] ?? (en as Dict)[key] ?? key;
  if (vars) s = s.replace(/\{(\w+)\}/g, (m, name: string) => (name in vars ? String(vars[name]) : m));
  return s;
}

/** A count with its plural form: `<key>.one` for 1, `<key>.other` otherwise. */
export function plural(lang: Lang, key: string, n: number, vars?: Record<string, string | number>): string {
  return translate(lang, `${key}.${n === 1 ? 'one' : 'other'}`, { n, ...vars });
}
