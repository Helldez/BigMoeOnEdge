// The UI's words: one English dictionary, looked up by key. Values may name `{placeholders}`; a
// plural picks `<key>.one` or `<key>.other` by `n`.
import { en } from './en';

/** The locale dates, times and lists are written in, to match the words. */
export const LOCALE = 'en';

/** `key`'s text, its placeholders filled; the key itself when the dictionary lacks it. */
export function t(key: string, vars?: Record<string, string | number>): string {
  let s = (en as Record<string, string>)[key] ?? key;
  if (vars) s = s.replace(/\{(\w+)\}/g, (m, name: string) => (name in vars ? String(vars[name]) : m));
  return s;
}

/** A count with its plural form: `<key>.one` for 1, `<key>.other` otherwise. */
export function tp(key: string, n: number, vars?: Record<string, string | number>): string {
  return t(`${key}.${n === 1 ? 'one' : 'other'}`, { n, ...vars });
}
