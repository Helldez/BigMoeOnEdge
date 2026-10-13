import { describe, expect, it } from 'vitest';
import { ema, mibPerSec, pushSample, sparklinePoints, stallShare, tokPerSec } from './metrics';

describe('rates', () => {
  it('tok/s is 1000 / wall_ms', () => {
    expect(tokPerSec(200)).toBe(5);
    expect(tokPerSec(0)).toBe(0);
  });

  it('MiB/s scales read_mib by the token wall time', () => {
    expect(mibPerSec(50, 250)).toBe(200);
    expect(mibPerSec(10, 0)).toBe(0);
  });

  it('stall share is clamped to [0, 1]', () => {
    expect(stallShare(50, 200)).toBe(0.25);
    expect(stallShare(300, 200)).toBe(1);
    expect(stallShare(5, 0)).toBe(0);
  });
});

describe('ema', () => {
  it('starts at the first sample', () => {
    expect(ema(null, 10)).toBe(10);
  });

  it('moves a fraction alpha toward the sample', () => {
    expect(ema(10, 20, 0.5)).toBe(15);
    expect(ema(10, 20, 0.2)).toBeCloseTo(12);
  });

  it('converges on a steady input', () => {
    let v: number | null = null;
    for (let i = 0; i < 200; i++) v = ema(v, i === 0 ? 0 : 4);
    expect(v).toBeCloseTo(4, 5);
  });
});

describe('pushSample', () => {
  it('keeps at most max samples, dropping the oldest', () => {
    let h: number[] = [];
    for (let i = 1; i <= 5; i++) h = pushSample(h, i, 3);
    expect(h).toEqual([3, 4, 5]);
  });

  it('does not mutate its input', () => {
    const h = [1, 2];
    pushSample(h, 3, 10);
    expect(h).toEqual([1, 2]);
  });
});

describe('sparklinePoints', () => {
  it('maps the maximum to the top and zero to the bottom', () => {
    expect(sparklinePoints([0, 10], 100, 20)).toBe('0,20 100,0');
  });

  it('handles empty and single-sample series', () => {
    expect(sparklinePoints([], 100, 20)).toBe('');
    expect(sparklinePoints([5], 100, 20)).toBe('100,0');
  });
});
