// How fast a download goes and how long it has left, from the byte counts the server reported and
// when. Over a sliding window, so a stall or a burst moves the estimate without making it jump.
import { t } from './text';

export interface Sample {
  /** Milliseconds, any monotonic clock. */
  at: number;
  received: number;
}

/** Keep the samples of the last `windowMs`, plus the newest. */
export function addSample(samples: readonly Sample[], next: Sample, windowMs = 20_000): Sample[] {
  const kept = samples.filter((s) => next.at - s.at <= windowMs && s.received <= next.received);
  return [...kept, next];
}

/** Bytes per second over the window, or null until two samples a second apart exist. */
export function rate(samples: readonly Sample[]): number | null {
  if (samples.length < 2) return null;
  const a = samples[0];
  const b = samples[samples.length - 1];
  const dt = (b.at - a.at) / 1000;
  if (dt < 1) return null;
  return Math.max(0, (b.received - a.received) / dt);
}

/** Seconds left at the current rate, or null when unknown. */
export function secondsLeft(samples: readonly Sample[], total: number): number | null {
  const r = rate(samples);
  const last = samples[samples.length - 1];
  if (!r || !last || !(total > 0)) return null;
  return Math.max(0, (total - last.received) / r);
}

/** "about 12 min", "about 1 h 20 min", "less than a minute": a person's rounding. */
export function humanDuration(s: number): string {
  if (s < 60) return t('eta.underMinute');
  const min = Math.round(s / 60);
  if (min < 60) return t('eta.min', { min });
  const h = Math.floor(min / 60);
  const m = min % 60;
  return m ? t('eta.hMin', { h, min: m }) : t('eta.h', { h });
}
