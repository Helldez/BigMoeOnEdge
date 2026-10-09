import { describe, expect, it } from 'vitest';
import { coerceInput, displayValue } from './coerce';
import type { ParamSpec } from './types';

function spec(type: string, extra: Partial<ParamSpec> = {}): ParamSpec {
  return {
    key: 'k',
    label: 'K',
    type,
    group: 'g',
    group_label: 'G',
    level: 'basic',
    scope: 'session',
    help: '',
    flag: '--k',
    default: 0,
    ...extra,
  };
}

describe('coerceInput', () => {
  it('bool: passes booleans through and parses text', () => {
    expect(coerceInput(spec('bool'), true)).toEqual({ ok: true, value: true });
    expect(coerceInput(spec('bool'), 'off')).toEqual({ ok: true, value: false });
    expect(coerceInput(spec('bool'), 'maybe').ok).toBe(false);
  });

  it('int: returns a number, not a string', () => {
    const r = coerceInput(spec('int'), ' 42 ');
    expect(r).toEqual({ ok: true, value: 42 });
    if (r.ok) expect(typeof r.value).toBe('number');
  });

  it('int: rejects fractions and junk', () => {
    expect(coerceInput(spec('int'), '1.5').ok).toBe(false);
    expect(coerceInput(spec('int'), 'abc').ok).toBe(false);
    expect(coerceInput(spec('int'), '').ok).toBe(false);
  });

  it('int/float: enforces min and max', () => {
    const s = spec('int', { min: 1, max: 10 });
    expect(coerceInput(s, '0').ok).toBe(false);
    expect(coerceInput(s, '11').ok).toBe(false);
    expect(coerceInput(s, '10')).toEqual({ ok: true, value: 10 });
  });

  it('accepts "auto" only when the schema says so, as a string', () => {
    expect(coerceInput(spec('int', { accepts_auto: true }), 'AUTO')).toEqual({ ok: true, value: 'auto' });
    expect(coerceInput(spec('float', { accepts_auto: true }), 'auto')).toEqual({ ok: true, value: 'auto' });
    expect(coerceInput(spec('int'), 'auto').ok).toBe(false);
  });

  it('float: parses decimals and exponents', () => {
    expect(coerceInput(spec('float'), '0.7')).toEqual({ ok: true, value: 0.7 });
    expect(coerceInput(spec('float'), '.5')).toEqual({ ok: true, value: 0.5 });
    expect(coerceInput(spec('float'), '1e-3')).toEqual({ ok: true, value: 0.001 });
    expect(coerceInput(spec('float'), '1.2.3').ok).toBe(false);
  });

  it('choice: must be one of the listed values', () => {
    const s = spec('choice', { choices: [{ value: 'x', label: 'X' }, { value: 'y', label: 'Y' }] });
    expect(coerceInput(s, 'y')).toEqual({ ok: true, value: 'y' });
    expect(coerceInput(s, 'z').ok).toBe(false);
  });

  it('path: keeps the string as typed, empty included', () => {
    expect(coerceInput(spec('path'), 'C:/models/a b.gguf')).toEqual({ ok: true, value: 'C:/models/a b.gguf' });
    expect(coerceInput(spec('path'), '')).toEqual({ ok: true, value: '' });
  });

  it('unknown future types fall back to strings', () => {
    expect(coerceInput(spec('color'), 'red')).toEqual({ ok: true, value: 'red' });
  });
});

describe('displayValue', () => {
  it('stringifies stored values', () => {
    expect(displayValue(8)).toBe('8');
    expect(displayValue('auto')).toBe('auto');
    expect(displayValue(undefined)).toBe('');
  });
});
