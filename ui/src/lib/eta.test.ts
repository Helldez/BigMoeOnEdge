import { describe, expect, it } from 'vitest';
import { addSample, humanDuration, rate, secondsLeft } from './eta';

describe('eta', () => {
  it('measures the rate over the window and the time left', () => {
    let s = addSample([], { at: 0, received: 0 });
    s = addSample(s, { at: 5000, received: 50_000_000 });
    expect(rate(s)).toBe(10_000_000);
    expect(secondsLeft(s, 150_000_000)).toBe(10);
  });
  it('knows nothing from one sample, or from samples less than a second apart', () => {
    expect(rate([{ at: 0, received: 0 }])).toBeNull();
    expect(rate([{ at: 0, received: 0 }, { at: 500, received: 10 }])).toBeNull();
  });
  it('drops samples older than the window, and any from before a restart', () => {
    let s = addSample([], { at: 0, received: 0 });
    s = addSample(s, { at: 30_000, received: 100 });
    expect(s).toHaveLength(1);
    s = addSample([{ at: 0, received: 500 }], { at: 1000, received: 10 });
    expect(s).toHaveLength(1);
  });
  it('rounds like a person', () => {
    expect(humanDuration(20)).toBe('less than a minute');
    expect(humanDuration(12 * 60 + 10)).toBe('about 12 min');
    expect(humanDuration(80 * 60)).toBe('about 1 h 20 min');
    expect(humanDuration(120 * 60)).toBe('about 2 h');
  });
});
