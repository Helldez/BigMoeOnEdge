// Live metric math over `token` events. All inputs are the raw per-token fields.

/** Instantaneous tokens per second from one token's wall time; 0 when unmeasured. */
export function tokPerSec(wallMs: number): number {
  return wallMs > 0 ? 1000 / wallMs : 0;
}

/** Flash read rate in MiB/s for one token. */
export function mibPerSec(readMib: number, wallMs: number): number {
  return wallMs > 0 ? (readMib * 1000) / wallMs : 0;
}

/** Share of the token's wall time spent waiting on reads, clamped to [0, 1]. */
export function stallShare(stallMs: number, wallMs: number): number {
  if (wallMs <= 0) return 0;
  return Math.min(1, Math.max(0, stallMs / wallMs));
}

/**
 * Exponential moving average. `prev` null starts the series at the first sample, so the first
 * reading is not dragged toward zero.
 */
export function ema(prev: number | null, sample: number, alpha = 0.2): number {
  if (prev === null || !Number.isFinite(prev)) return sample;
  return prev + alpha * (sample - prev);
}

/** Appends to a bounded history, dropping the oldest samples. Returns a new array. */
export function pushSample(history: readonly number[], sample: number, max: number): number[] {
  const next = history.length >= max ? history.slice(history.length - max + 1) : history.slice();
  next.push(sample);
  return next;
}

/** SVG polyline points for a sparkline of `values` in a `width` x `height` box. */
export function sparklinePoints(values: readonly number[], width: number, height: number): string {
  if (values.length === 0) return '';
  const max = Math.max(...values);
  const min = Math.min(0, ...values);
  const span = max - min || 1;
  const step = values.length > 1 ? width / (values.length - 1) : 0;
  return values
    .map((v, i) => {
      const x = values.length > 1 ? i * step : width;
      const y = height - ((v - min) / span) * height;
      return `${round(x)},${round(y)}`;
    })
    .join(' ');
}

function round(n: number): number {
  return Math.round(n * 10) / 10;
}
