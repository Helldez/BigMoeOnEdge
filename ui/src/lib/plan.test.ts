import { describe, expect, it } from 'vitest';
import { appliedChanges, deviceTone, diagnosticText, groupDecisions, memorySegments, rateScale, speedCheck } from './plan';
import type { ParamSpec, PlanDecision, PlanDevice, PlanMemoryPool } from './types';

describe('appliedChanges', () => {
  const row = (key: string, extra: Partial<ParamSpec> = {}): ParamSpec => ({
    key,
    label: `Label ${key}`,
    type: 'int',
    group: 'g',
    group_label: 'G',
    level: 'basic',
    scope: 'session',
    help: '',
    flag: `--${key}`,
    default: 0,
    ...extra,
  });
  const table = [
    row('size', { unit: 'MiB', special: [{ value: 0, label: 'off' }] }),
    row('mode', { type: 'choice', choices: [{ value: 'a', label: 'first' }, { value: 'b', label: 'second' }] }),
    row('same'),
  ];

  it('lists the rows whose value changed, in table order, worded from the table', () => {
    const out = appliedChanges({ size: 0, mode: 'a', same: 3 }, { size: 703, mode: 'b', same: '3' }, table);
    expect(out).toEqual([
      { key: 'size', label: 'Label size', from: 'off', to: '703 MiB' },
      { key: 'mode', label: 'Label mode', from: 'first', to: 'second' },
    ]);
  });

  it('keeps a key the table does not have under its own name', () => {
    expect(appliedChanges({}, { other: 2 }, table)).toEqual([{ key: 'other', label: 'other', from: '', to: '2' }]);
  });
});

describe('diagnosticText', () => {
  it('is the command, then the account', () => {
    expect(diagnosticText(['--threads', '8'], ' plan: x \n')).toBe('bmoe-cli --threads 8\n\nplan: x\n');
  });
});

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
  it('keeps no grouping policy of its own: an ungrouped decision is with the set ones', () => {
    const g = groupDecisions([dec('storage-rate', 'derived'), dec('a', 'operator')]);
    expect([g.set.length, g.kept.length, g.notes.length]).toEqual([2, 0, 0]);
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
  it('draws every held row in the server words, then the free part, as shares of the cap', () => {
    const s = memorySegments(pool(10, [
      { name: 'dense', label: 'Dense', bytes: 2, locked: true, in_turn: false },
      { name: 'expert cache', bytes: 6, locked: false, in_turn: false },
    ]));
    expect(s.map((x) => [x.label, x.frac, x.slot])).toEqual([
      ['Dense', 0.2, 0],
      ['expert cache', 0.6, 1],
      ['Free', 0.2, -1],
    ]);
    expect(s[0].locked).toBe(true);
  });
  it('leaves out what is held in turn with the cache', () => {
    const s = memorySegments(pool(4, [
      { name: 'expert cache', bytes: 4, locked: false, in_turn: false },
      { name: 'device slots', bytes: 1, locked: false, in_turn: true },
    ]));
    expect(s.map((x) => x.label)).toEqual(['expert cache']);
  });
  it('still draws a ledger that does not close', () => {
    const s = memorySegments(pool(4, [{ name: 'dense', bytes: 8, locked: false, in_turn: false }]));
    expect(s).toHaveLength(1);
    expect(s[0].frac).toBe(1);
  });
});

describe('deviceTone', () => {
  it('colours by role alone', () => {
    expect(deviceTone(dev({ status: 'used', uses: ['prompt'] }))).toBe('ok');
    expect(deviceTone(dev({ status: 'refused' }))).toBe('err');
    expect(deviceTone(dev({ status: 'unusable' }))).toBe('err');
    expect(deviceTone(dev({ status: 'unused' }))).toBe('');
    expect(deviceTone(dev({ status: 'a role added later' }))).toBe('');
  });
});

describe('rateScale', () => {
  it('is the fastest device, never below the host', () => {
    expect(rateScale([dev({ prompt_ratio: 1 }), dev({ prompt_ratio: 40 })])).toBe(40);
    expect(rateScale([dev({ prompt_ratio: 0.5 })])).toBe(1);
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
