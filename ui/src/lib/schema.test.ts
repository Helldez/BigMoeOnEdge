import { describe, expect, it } from 'vitest';
import {
  advancedGroups,
  groupParams,
  helpParts,
  isDefault,
  isLevelVisible,
  sameValue,
  specialLabel,
  typicalText,
} from './schema';
import type { ParamSpec } from './types';

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

describe('groupParams', () => {
  it('shows only basic with no optional level enabled', () => {
    const g = groupParams(rows, new Set());
    expect(g.map((x) => x.group)).toEqual(['g1', 'g2']);
    expect(g[0].params.map((p) => p.key)).toEqual(['a']);
    expect(g[1].params.map((p) => p.key)).toEqual(['e']);
  });

  it('keeps group first-appearance order and schema order within a group', () => {
    const g = groupParams(rows, new Set(['advanced', 'experimental', 'debug']));
    expect(g.map((x) => x.group)).toEqual(['g1', 'g2', 'g3']);
    expect(g[0].params.map((p) => p.key)).toEqual(['a', 'c']);
    expect(g[1].params.map((p) => p.key)).toEqual(['b', 'e']);
    expect(g[0].label).toBe('g1 label');
  });

  it('drops groups with no visible parameter', () => {
    const g = groupParams(rows, new Set(['debug']));
    expect(g.map((x) => x.group)).toEqual(['g1', 'g2']);
    expect(g[0].params.map((p) => p.key)).toEqual(['a', 'c']);
  });

  it('falls back to the group key when group_label is empty', () => {
    const g = groupParams([spec('x', 'raw', 'basic', { group_label: '' })], new Set());
    expect(g[0].label).toBe('raw');
  });
});

describe('levels', () => {
  it('basic is always visible', () => {
    expect(isLevelVisible('basic', new Set())).toBe(true);
    expect(isLevelVisible('advanced', new Set())).toBe(false);
    expect(isLevelVisible('advanced', new Set(['advanced']))).toBe(true);
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

describe('advancedGroups', () => {
  it('holds every row but the basic ones, so no row appears twice', () => {
    const g = advancedGroups(rows);
    const keys = g.flatMap((x) => x.params.map((p) => p.key));
    expect(keys).toEqual(['c', 'b', 'd']);
    const basic = groupParams(rows, new Set()).flatMap((x) => x.params.map((p) => p.key));
    expect(keys.filter((k) => basic.includes(k))).toEqual([]);
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

