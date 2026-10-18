import { describe, expect, it } from 'vitest';
import { deviceBadge, devicesNoneText, groupDecisions, memorySegments, rateScale, speedCheck } from './plan';
import type { PlanDecision, PlanDevice, PlanMemoryPool } from './types';

const dec = (knob: string, source: string, group?: string): PlanDecision => ({ knob, value: '1', source, reason: 'r', group });

const dev = (over: Partial<PlanDevice>): PlanDevice => ({
  name: 'D0',
  description: '',
  cpu: false,
  own_memory: false,
  memory_total: 0,
  prompt_ratio: 0,
  token_ratio: 0,
  uses: [],
  status: 'unused',
  why: '',
  ...over,
});

describe('groupDecisions', () => {
  it('follows the server group', () => {
    const g = groupDecisions([dec('a', 'measured', 'set'), dec('b', 'derived', 'kept'), dec('regime', 'derived', 'notes')]);
    expect([g.set.length, g.kept.length, g.notes.length]).toEqual([1, 1, 1]);
  });
  it('falls back on knob and source for a server without groups', () => {
    const g = groupDecisions([dec('cost', 'derived'), dec('a', 'operator'), dec('b', 'unprobed'), dec('c', 'measured')]);
    expect(g.notes.map((d) => d.knob)).toEqual(['cost']);
    expect(g.kept.map((d) => d.knob)).toEqual(['a', 'b']);
    expect(g.set.map((d) => d.knob)).toEqual(['c']);
  });
});

describe('memorySegments', () => {
  const pool = (cap: number, rows: PlanMemoryPool['rows']): PlanMemoryPool => ({
    name: 'host',
    cap_bytes: cap,
    lockable_cap_bytes: 0,
    held_bytes: 0,
    locked_bytes: 0,
    rows,
  });
  it('draws every held row, then the free part, as shares of the cap', () => {
    const s = memorySegments(pool(10, [
      { name: 'dense', bytes: 2, locked: true, in_turn: false },
      { name: 'expert cache', bytes: 6, locked: false, in_turn: false },
    ]));
    expect(s.map((x) => [x.name, x.frac, x.slot])).toEqual([
      ['dense', 0.2, 0],
      ['expert cache', 0.6, 1],
      ['free', 0.2, -1],
    ]);
    expect(s[0].locked).toBe(true);
  });
  it('leaves out what is held in turn with the cache', () => {
    const s = memorySegments(pool(4, [
      { name: 'expert cache', bytes: 4, locked: false, in_turn: false },
      { name: 'device slots', bytes: 1, locked: false, in_turn: true },
    ]));
    expect(s.map((x) => x.name)).toEqual(['expert cache']);
  });
  it('still draws a ledger that does not close', () => {
    const s = memorySegments(pool(4, [{ name: 'dense', bytes: 8, locked: false, in_turn: false }]));
    expect(s).toHaveLength(1);
    expect(s[0].frac).toBe(1);
  });
});

describe('deviceBadge', () => {
  it('names what a used device does', () => {
    expect(deviceBadge(dev({ cpu: true, status: 'used', uses: ['prompt', 'tokens'] }))).toEqual({ text: 'Prompt and tokens', tone: 'ok' });
    expect(deviceBadge(dev({ status: 'used', uses: ['layers', 'prompt'] })).text).toBe('Layers and prompt');
  });
  it('says why the others do nothing', () => {
    expect(deviceBadge(dev({ status: 'refused' })).tone).toBe('err');
    expect(deviceBadge(dev({ status: 'unusable' })).text).toBe('Unusable');
    expect(deviceBadge(dev({ status: 'unused' }))).toEqual({ text: 'Not used', tone: '' });
  });
});

describe('rateScale', () => {
  it('is the fastest device, never below the host', () => {
    expect(rateScale([dev({ prompt_ratio: 1 }), dev({ prompt_ratio: 40 })])).toBe(40);
    expect(rateScale([dev({ prompt_ratio: 0.5 })])).toBe(1);
  });
});

describe('devicesNoneText', () => {
  it('keeps the three answers apart', () => {
    const t = ['build', 'machine', 'not_probed'].map(devicesNoneText);
    expect(new Set(t).size).toBe(3);
    expect(devicesNoneText('')).toBe('');
  });
});

describe('speedCheck', () => {
  it('reads as bmoe-cli prints it', () => {
    const c = speedCheck({ s_per_token: 0.302, cache_hit_pct: 38 }, { s_per_token: 0.24, cache_hit_pct: 63, tokens: 50 });
    expect(c.deltaPct).toBeCloseTo(25.8, 1);
    expect(c.measuredTokS).toBeCloseTo(4.17, 2);
    expect(c.predictedHitPct).toBe(38);
    expect(c.measuredHitPct).toBe(63);
  });
  it('has no measured figures before a turn, and no hit rate without a cache', () => {
    const c = speedCheck({ s_per_token: 0.5 }, undefined);
    expect(c.measuredTokS).toBeNull();
    expect(c.deltaPct).toBeNull();
    expect(speedCheck(undefined, { s_per_token: 1, cache_hit_pct: -1, tokens: 1 }).measuredHitPct).toBeNull();
  });
});
