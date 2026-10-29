// A wire time in words for a list: "in 5 min", "2 h ago", or the date when it is far.
import { timeMs, type WireTime } from './agent';

export function relativeTime(t: WireTime, now: number, locale: string): string {
  const ms = timeMs(t);
  if (Number.isNaN(ms)) return '';
  const diff = ms - now;
  const abs = Math.abs(diff);
  const rtf = new Intl.RelativeTimeFormat(locale, { numeric: 'auto', style: 'short' });
  const MIN = 60_000;
  const HOUR = 60 * MIN;
  const DAY = 24 * HOUR;
  if (abs < MIN) return rtf.format(Math.round(diff / 1000), 'second');
  if (abs < HOUR) return rtf.format(Math.round(diff / MIN), 'minute');
  if (abs < DAY) return rtf.format(Math.round(diff / HOUR), 'hour');
  if (abs < 7 * DAY) return rtf.format(Math.round(diff / DAY), 'day');
  return new Intl.DateTimeFormat(locale, { dateStyle: 'medium', timeStyle: 'short' }).format(new Date(ms));
}
