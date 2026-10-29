import { describe, expect, it } from 'vitest';
import { describeSchedule, scheduleProblem, weekdayNames } from './schedule';
import { t } from './text';

describe('describeSchedule', () => {
  it('says an interval in its largest whole unit', () => {
    expect(describeSchedule({ kind: 'interval', every_minutes: 30 }, t, 'en')).toBe('Every 30 minutes');
    expect(describeSchedule({ kind: 'interval', every_minutes: 1 }, t, 'en')).toBe('Every minute');
    expect(describeSchedule({ kind: 'interval', every_minutes: 120 }, t, 'en')).toBe('Every 2 hours');
    expect(describeSchedule({ kind: 'interval', every_minutes: 60 }, t, 'en')).toBe('Every hour');
    expect(describeSchedule({ kind: 'interval', every_minutes: 2880 }, t, 'en')).toBe('Every 2 days');
  });

  it('says a daily and a weekly time', () => {
    expect(describeSchedule({ kind: 'daily', at: '09:00' }, t, 'en')).toBe('Every day at 09:00');
    expect(describeSchedule({ kind: 'weekly', at: '08:30', weekdays: [5, 1] }, t, 'en')).toBe('Every Monday and Friday at 08:30');
    expect(describeSchedule({ kind: 'weekly', at: '08:30', weekdays: [1, 2, 3, 4, 5] }, t, 'en')).toBe('Every weekday at 08:30');
    expect(describeSchedule({ kind: 'weekly', at: '10:00', weekdays: [0, 6] }, t, 'en')).toBe('Every weekend at 10:00');
    expect(describeSchedule({ kind: 'weekly', at: '07:00', weekdays: [0, 1, 2, 3, 4, 5, 6] }, t, 'en')).toBe('Every day at 07:00');
  });

  it('says a one-off date in the locale', () => {
    const s = describeSchedule({ kind: 'once', at: '2026-11-03T09:00:00' }, t, 'en');
    expect(s.startsWith('Once, ')).toBe(true);
    expect(s).toContain('2026');
  });


  it('says what is missing rather than inventing a time', () => {
    expect(describeSchedule({ kind: 'once', at: '' }, t, 'en')).toBe('Once, date not set');
    expect(describeSchedule({ kind: 'weekly', at: '09:00', weekdays: [] }, t, 'en')).toBe('Weekly, no day picked');
  });
});

describe('scheduleProblem', () => {
  it('accepts what can run and names what cannot', () => {
    expect(scheduleProblem({ kind: 'daily', at: '09:00' })).toBe('');
    expect(scheduleProblem({ kind: 'daily', at: '9am' })).toBe('schedule.problem.time');
    expect(scheduleProblem({ kind: 'interval', every_minutes: 0 })).toBe('schedule.problem.every');
    expect(scheduleProblem({ kind: 'weekly', at: '09:00', weekdays: [] })).toBe('schedule.problem.days');
    expect(scheduleProblem({ kind: 'once', at: 'soon' })).toBe('schedule.problem.when');
    expect(scheduleProblem({ kind: 'once', at: '2026-11-03T09:00' })).toBe('');
  });
});

describe('weekdayNames', () => {
  it('starts on Sunday', () => {
    expect(weekdayNames('en')[0]).toBe('Sunday');
    expect(weekdayNames('en', 'short')[1]).toBe('Mon');
  });
});
