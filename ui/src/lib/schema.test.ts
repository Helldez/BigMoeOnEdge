import { describe, expect, it } from 'vitest';
import {
  differsFromPlan,
  helpParts,
  isDefault,
  levelCounts,
  levelOf,
  resettableKeys,
  sameValue,
  settingsCards,
  specialLabel,
  typicalText,
  valueSource,
} from './schema';
import type { ParamSpec, PlanDecision } from './types';

// Synthetic rows: the tests must not depend on the engine's real parameter table.
function spec(key: string, group: string, level: string, extra: Partial<ParamSpec> = {}): ParamSpec {
  return {
    key,
    label: key.toUpperCase(),
    type: 'int',
    group,
    group_label: `${group} label`,
    level,
    scope: 'session',
    help: '',
    flag: `--${key}`,
    default: 0,
    ...extra,
  };
}

const rows = [
  spec('a', 'g1', 'basic'),
  spec('b', 'g2', 'advanced'),
  spec('c', 'g1', 'debug'),
  spec('d', 'g3', 'experimental'),
  spec('e', 'g2', 'basic'),
];

const keys = (ps: ParamSpec[]) => ps.map((p) => p.key);

describe('settingsCards', () => {
  it('gives every group one card, its basic rows open and the rest folded by level', () => {
    const c = settingsCards(rows, { upTo: 'debug' });
    expect(c.map((x) => x.group)).toEqual(['g1', 'g2', 'g3']);
    expect(keys(c[0].basic)).toEqual(['a']);
    expect(c[0].more.map((m) => [m.level, keys(m.params)])).toEqual([['debug', ['c']]]);
    expect(keys(c[1].basic)).toEqual(['e']);
    expect(c[1].more.map((m) => m.level)).toEqual(['advanced']);
    expect(c[2].basic).toEqual([]);
    expect(c[0].label).toBe('g1 label');
  });

  it('shows each row once', () => {
    const c = settingsCards(rows, { upTo: 'debug' });
    const all = c.flatMap((x) => [...keys(x.basic), ...x.more.flatMap((m) => keys(m.params))]);
    expect(all.sort()).toEqual(['a', 'b', 'c', 'd', 'e']);
  });

  it('leaves out the levels below the filter, counts them, and drops a card left empty', () => {
    const c = settingsCards(rows, { upTo: 'basic' });
    expect(c.map((x) => x.group)).toEqual(['g1', 'g2']);
    expect(c[0].more).toEqual([]);
    expect(c[0].hidden).toBe(1);
    const adv = settingsCards(rows, { upTo: 'advanced' });
    expect(adv.map((x) => x.group)).toEqual(['g1', 'g2']);
    expect(adv[1].more.map((m) => m.level)).toEqual(['advanced']);
  });

  it('shows a pinned row whatever the filter', () => {
    const c = settingsCards(rows, { upTo: 'basic', pinned: new Set(['d']) });
    expect(c.map((x) => x.group)).toEqual(['g1', 'g2', 'g3']);
    expect(keys(c[2].more[0].params)).toEqual(['d']);
  });

  it('orders cards by the table group order, not by where a group first appears', () => {
    const ordered = [
      spec('x', 'late', 'basic', { group_order: 2 }),
      spec('y', 'early', 'basic', { group_order: 0 }),
      spec('z', 'mid', 'basic', { group_order: 1 }),
    ];
    expect(settingsCards(ordered, { upTo: 'basic' }).map((x) => x.group)).toEqual(['early', 'mid', 'late']);
  });

  it('carries the group help and the read-only mark from the table', () => {
    const c = settingsCards([spec('m', 'picked', 'basic', { group_help: 'One line.', group_read_only: true })], {
      upTo: 'basic',
    });
    expect(c[0].help).toBe('One line.');
    expect(c[0].readOnly).toBe(true);
  });

  it('searches every level, ignoring the filter', () => {
    const found = settingsCards([...rows, spec('needle', 'g3', 'debug', { label: 'Find me' })], {
      upTo: 'basic',
      query: 'find ME',
    });
    expect(found.map((x) => x.group)).toEqual(['g3']);
    expect(keys(found[0].more[0].params)).toEqual(['needle']);
    expect(settingsCards(rows, { upTo: 'debug', query: 'nothing like it' })).toEqual([]);
  });

  it('falls back to the group key when group_label is empty', () => {
    const g = settingsCards([spec('x', 'raw', 'basic', { group_label: '' })], { upTo: 'basic' });
    expect(g[0].label).toBe('raw');
  });
});

describe('levels', () => {
  it('counts the rows of each level and finds a row\'s level', () => {
    expect(levelCounts(rows).get('basic')).toBe(2);
    expect(levelOf(rows, 'd')).toBe('experimental');
    expect(levelOf(rows, 'nope')).toBeUndefined();
  });
});

describe('resettableKeys', () => {
  it('leaves launch keys and read-only rows, and keeps the rest of what the person set', () => {
    const table = [
      spec('file', 'picked', 'basic', { group_read_only: true }),
      spec('threads', 'g', 'basic'),
      spec('temp', 'g', 'basic'),
    ];
    expect(resettableKeys(['file', 'threads', 'temp'], ['threads'], table)).toEqual(['temp']);
    expect(resettableKeys(['file'], [], table)).toEqual([]);
    expect(resettableKeys(['temp', 'unknown'], [], table)).toEqual(['temp', 'unknown']);
  });
});

describe('where a value came from', () => {
  const planned: PlanDecision = { knob: 'k', value: '703', source: 'derived', reason: '', group: 'set' };
  const kept: PlanDecision = { knob: 'k', value: '8', source: 'operator', reason: '', group: 'kept' };

  it('is the person, else the plan that decided it, else the default', () => {
    expect(valueSource(true, planned)).toBe('you');
    expect(valueSource(false, planned)).toBe('plan');
    expect(valueSource(false, kept)).toBe('default');
    expect(valueSource(false, undefined)).toBe('default');
  });

  it('differs from the plan only where the plan decided a different value', () => {
    expect(differsFromPlan(703, planned)).toBe(false);
    expect(differsFromPlan(512, planned)).toBe(true);
    expect(differsFromPlan(true, { ...planned, value: 'true' })).toBe(false);
    expect(differsFromPlan(512, kept)).toBe(false);
    expect(differsFromPlan(512, undefined)).toBe(false);
  });
});

describe('default comparison', () => {
  it('compares across JSON types', () => {
    expect(sameValue(8, 8)).toBe(true);
    expect(sameValue(8, '8')).toBe(true);
    expect(sameValue('auto', 'auto')).toBe(true);
    expect(sameValue('auto', 0)).toBe(false);
    expect(sameValue(true, 'true')).toBe(false);
    expect(sameValue('', 0)).toBe(false);
  });

  it('isDefault uses the schema default', () => {
    const s = spec('k', 'g', 'basic', { default: 'auto', accepts_auto: true });
    expect(isDefault(s, 'auto')).toBe(true);
    expect(isDefault(s, 4096)).toBe(false);
  });
});

describe('special values and the typical range', () => {
  const lanes = spec('lanes', 'g', 'advanced', {
    unit: 'lanes',
    typical: { min: 2, max: 8 },
    special: [{ value: 0, label: 'off' }],
  });

  it('names a special value whatever JSON type carries it', () => {
    expect(specialLabel(lanes, 0)).toBe('off');
    expect(specialLabel(lanes, '0')).toBe('off');
    expect(specialLabel(lanes, 4)).toBeUndefined();
    expect(specialLabel(spec('x', 'g', 'basic'), 0)).toBeUndefined();
  });

  it('says the typical range in words', () => {
    expect(typicalText(lanes)).toBe('usually 2 to 8');
    expect(typicalText(spec('x', 'g', 'basic', { typical: { min: 3, max: 3 } }))).toBe('usually 3');
    expect(typicalText(spec('x', 'g', 'basic'))).toBe('');
  });
});

describe('helpParts', () => {
  it('keeps the first sentence and folds the rest', () => {
    expect(helpParts('Parallel expert-read lanes, including the calling thread. 1 is the serial baseline.')).toEqual({
      lead: 'Parallel expert-read lanes, including the calling thread.',
      rest: '1 is the serial baseline.',
    });
  });

  it('does not split on an abbreviation or a short opening', () => {
    expect(helpParts('Run it on this device (e.g. HTP0) while decode stays.').rest).toBe('');
    expect(helpParts('Compute threads.').lead).toBe('Compute threads.');
  });
});

