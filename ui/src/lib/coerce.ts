// Converts what a form control holds into the JSON value the server expects for a parameter,
// following the parameter's schema row: numbers as numbers, bools as bools, "auto" as a string.
import type { ParamSpec, ParamValue } from './types';

export type Coerced = { ok: true; value: ParamValue } | { ok: false; error: string };

const INT_RE = /^[+-]?\d+$/;
const FLOAT_RE = /^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/;

export function isAuto(raw: string): boolean {
  return raw.trim().toLowerCase() === 'auto';
}

function checkRange(spec: ParamSpec, n: number): Coerced {
  if (spec.min !== undefined && n < spec.min) return { ok: false, error: `must be at least ${spec.min}` };
  if (spec.max !== undefined && n > spec.max) return { ok: false, error: `must be at most ${spec.max}` };
  return { ok: true, value: n };
}

export function coerceInput(spec: ParamSpec, raw: string | boolean): Coerced {
  if (spec.type === 'bool') {
    if (typeof raw === 'boolean') return { ok: true, value: raw };
    const s = raw.trim().toLowerCase();
    if (s === 'true' || s === '1' || s === 'on') return { ok: true, value: true };
    if (s === 'false' || s === '0' || s === 'off') return { ok: true, value: false };
    return { ok: false, error: 'expected on or off' };
  }

  const text = String(raw);
  if (spec.type === 'int' || spec.type === 'float') {
    const s = text.trim();
    if (spec.accepts_auto && isAuto(s)) return { ok: true, value: 'auto' };
    if (s === '') return { ok: false, error: spec.accepts_auto ? 'enter a number or "auto"' : 'enter a number' };
    if (spec.type === 'int') {
      if (!INT_RE.test(s)) return { ok: false, error: 'must be a whole number' };
      return checkRange(spec, parseInt(s, 10));
    }
    if (!FLOAT_RE.test(s)) return { ok: false, error: 'must be a number' };
    return checkRange(spec, parseFloat(s));
  }

  if (spec.type === 'choice' && spec.choices && spec.choices.length > 0) {
    if (!spec.choices.some((c) => c.value === text)) return { ok: false, error: 'not one of the choices' };
    return { ok: true, value: text };
  }

  // path, choice without a list, and any future string-typed parameter.
  return { ok: true, value: text };
}

/** The text a form control shows for a stored value. */
export function displayValue(value: ParamValue | undefined): string {
  if (value === undefined || value === null) return '';
  return String(value);
}
