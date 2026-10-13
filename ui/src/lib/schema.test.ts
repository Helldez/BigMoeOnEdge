import { describe, expect, it } from 'vitest';
import { countByLevel, groupParams, isDefault, isLevelVisible, sameValue } from './schema';
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

  it('counts per level', () => {
    expect(countByLevel(rows)).toEqual({ basic: 2, advanced: 1, debug: 1, experimental: 1 });
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
