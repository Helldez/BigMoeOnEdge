import { describe, expect, it } from 'vitest';
import { isDoneSummary } from './guards';
import { timeShares, turnFigures } from './turnMetrics';
import type { DoneSummary } from './types';

const s: DoneSummary = {
  cancelled: false,
  tokens: 42,
  tok_s: 10,
  prefill_s: 1.25,
  prefill_tps: 13.6,
  n_prompt: 17,
  n_past: 900,
  cache_hit_pct: 81.4,
  read_mib: 0,
  io_s_tok: 0,
  compute_s_tok: 0.07,
  stall_s_tok: 0,
  flash_wait_s_tok: 0.015,
  mgmt_s_tok: 0.002,
  cache_resident_mib: 0,
  cache_budget_mib: 0,
  ttft_s: 1.25,
};

describe('timeShares', () => {
  it('splits a token\'s time into whole percentages that add up to 100', () => {
    const t = timeShares(s);
    expect(t.map((p) => p.key)).toEqual(['compute', 'disk', 'mgmt', 'other']);
    expect(t.reduce((a, p) => a + p.pct, 0)).toBe(100);
    expect(t.find((p) => p.key === 'compute')?.pct).toBe(70);
  });
  it('adds up to 100 even when every part rounds down', () => {
    const odd = { ...s, tok_s: 3, compute_s_tok: 0.111, flash_wait_s_tok: 0.111, mgmt_s_tok: 0.111 };
    expect(timeShares(odd).reduce((a, p) => a + p.pct, 0)).toBe(100);
  });
  it('is empty without a rate', () => {
    expect(timeShares({ ...s, tok_s: 0 })).toEqual([]);
  });
});

describe('turnFigures', () => {
  it('says the time split once, adding up to 100, and what was in memory as a count', () => {
    const f = turnFigures(s, 16384, 13.9);
    expect(f[0].value).toBe('10.0 tok/s, plan expected 13.9');
    expect(f.find((x) => x.id === 'time')?.value).toBe('computing 70% · waiting for the disk 15% · managing the cache 2% · other 13%');
    expect(f.find((x) => x.id === 'cache')?.value).toBe('81 of every 100 parts');
    expect(f.find((x) => x.id === 'context')?.value).toBe('900 of 16,384 tokens');
    expect(f.find((x) => x.id === 'prompt')?.value).toBe('13.6 tok/s, 17 tokens in 1.3 s');
    // No figure but the time split is a percentage.
    expect(f.filter((x) => x.id !== 'time' && /%/.test(x.value))).toEqual([]);
    for (const x of f) expect(x.hint).not.toBe('');
  });

  it('says nothing of a plan it does not have, a cache it does not use, or a stop that did not happen', () => {
    const f = turnFigures({ ...s, cache_hit_pct: -1 }, 0, null);
    expect(f[0].value).toBe('10.0 tok/s');
    expect(f.map((x) => x.id)).toEqual(['speed', 'time', 'prompt']);
    expect(turnFigures({ ...s, cancelled: true }, 0, null).at(-1)?.id).toBe('stopped');
  });
});

describe('a resident model (nothing streamed)', () => {
  // A `done` event as bmoe-server sends it for a model held whole in memory: no flash or cache
  // terms, the whole token in compute, and both speeds measured.
  const resident: DoneSummary = {
    ...s,
    tok_s: 93.9,
    prefill_s: 0.17,
    prefill_tps: 111.6,
    n_prompt: 19,
    n_past: 25,
    cache_hit_pct: -1,
    read_mib: 0,
    compute_s_tok: 1 / 93.9,
    flash_wait_s_tok: 0,
    mgmt_s_tok: 0,
  };
  it('passes the guard every screen reads the event through', () => {
    expect(isDoneSummary(resident)).toBe(true);
  });
  it('still says how fast it wrote and read the prompt, and that the time went to computing', () => {
    const f = turnFigures(resident, 16384, null);
    expect(f.map((x) => x.id)).toEqual(['speed', 'time', 'prompt', 'context']);
    expect(f[0].value).toBe('93.9 tok/s');
    expect(f.find((x) => x.id === 'prompt')?.value).toBe('111.6 tok/s, 19 tokens in 0.2 s');
    expect(timeShares(resident)).toEqual([{ key: 'compute', pct: 100 }]);
  });
});
