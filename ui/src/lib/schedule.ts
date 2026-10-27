// A task's schedule (docs/agent.md, "Tasks") in plain words, and the checks a form runs before it
// sends one. The words come from the dictionary; day names and dates from the browser's locale.
import { timeMs, type Schedule } from './agent';
import type { Translate } from './items';

/** Day names in `locale`, 0 = Sunday, as the schedule numbers them. */
export function weekdayNames(locale: string, width: 'long' | 'short' = 'long'): string[] {
  const fmt = new Intl.DateTimeFormat(locale, { weekday: width, timeZone: 'UTC' });
  // 2023-01-01 was a Sunday.
  return Array.from({ length: 7 }, (_, i) => fmt.format(new Date(Date.UTC(2023, 0, 1 + i))));
}

/** Monday first, as most calendars outside North America list a week. */
export const WEEK_ORDER = [1, 2, 3, 4, 5, 6, 0];

function joinList(parts: string[], locale: string): string {
  try {
    return new Intl.ListFormat(locale, { style: 'long', type: 'conjunction' }).format(parts);
  } catch {
    return parts.join(', ');
  }
}

function minutesText(m: number, t: Translate): string {
  if (m % 1440 === 0) {
    const d = m / 1440;
    return t(d === 1 ? 'schedule.every.day.one' : 'schedule.every.day.other', { n: d });
  }
  if (m % 60 === 0) {
    const h = m / 60;
    return t(h === 1 ? 'schedule.every.hour.one' : 'schedule.every.hour.other', { n: h });
  }
  return t(m === 1 ? 'schedule.every.minute.one' : 'schedule.every.minute.other', { n: m });
}

/** "Every day at 09:00", "Every Monday and Friday at 08:30", "Once, 3 Nov 2026, 09:00", "Every 2 hours". */
export function describeSchedule(s: Schedule, t: Translate, locale: string): string {
  switch (s.kind) {
    case 'once': {
      const ms = timeMs(s.at ?? '');
      if (Number.isNaN(ms)) return t('schedule.once.unset');
      const when = new Intl.DateTimeFormat(locale, { dateStyle: 'medium', timeStyle: 'short' }).format(new Date(ms));
      return t('schedule.once', { when });
    }
    case 'interval':
      return s.every_minutes && s.every_minutes > 0 ? minutesText(s.every_minutes, t) : t('schedule.interval.unset');
    case 'daily':
      return t('schedule.daily', { time: s.at || '00:00' });
    case 'weekly': {
      const days = [...new Set(s.weekdays ?? [])].filter((d) => d >= 0 && d <= 6);
      if (days.length === 0) return t('schedule.weekly.unset');
      const time = s.at || '00:00';
      if (days.length === 7) return t('schedule.daily', { time });
      const set = new Set(days);
      if (days.length === 5 && [1, 2, 3, 4, 5].every((d) => set.has(d))) return t('schedule.weekdays', { time });
      if (days.length === 2 && set.has(0) && set.has(6)) return t('schedule.weekends', { time });
      const names = weekdayNames(locale);
      const list = joinList(WEEK_ORDER.filter((d) => set.has(d)).map((d) => names[d]), locale);
      return t('schedule.weekly', { days: list, time });
    }
    default:
      return String(s.kind);
  }
}

const HHMM = /^([01]\d|2[0-3]):[0-5]\d$/;

/** Why a schedule cannot be saved, as a dictionary key; '' when it can. */
export function scheduleProblem(s: Schedule): string {
  switch (s.kind) {
    case 'once':
      return Number.isNaN(timeMs(s.at ?? '')) ? 'schedule.problem.when' : '';
    case 'interval':
      return !s.every_minutes || s.every_minutes < 1 || !Number.isInteger(s.every_minutes) ? 'schedule.problem.every' : '';
    case 'daily':
      return HHMM.test(s.at ?? '') ? '' : 'schedule.problem.time';
    case 'weekly':
      if (!HHMM.test(s.at ?? '')) return 'schedule.problem.time';
      return (s.weekdays ?? []).length === 0 ? 'schedule.problem.days' : '';
    default:
      return 'schedule.problem.kind';
  }
}
