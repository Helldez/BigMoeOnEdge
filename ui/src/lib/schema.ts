// Schema helpers: everything the settings form knows about parameters comes from here, and all
// of it is derived from the `GET /api/params` rows. No parameter is named in this module.
import type { ParamLevel, ParamSpec, ParamValue } from './types';

/** Optional levels in display order; `basic` is always shown. */
export const OPTIONAL_LEVELS: readonly ParamLevel[] = ['advanced', 'experimental', 'debug'];

export interface ParamGroup {
  group: string;
  label: string;
  params: ParamSpec[];
}

/** True when a parameter of this level is visible with the given optional levels enabled. */
export function isLevelVisible(level: string, enabled: ReadonlySet<string>): boolean {
  return level === 'basic' || enabled.has(level);
}

/**
 * Groups visible parameters by `group`, keeping the order in which groups first appear in the
 * schema (the form order) and the schema order of parameters inside each group. Empty groups
 * are dropped.
 */
export function groupParams(params: readonly ParamSpec[], enabled: ReadonlySet<string>): ParamGroup[] {
  const groups = new Map<string, ParamGroup>();
  for (const p of params) {
    if (!isLevelVisible(p.level, enabled)) continue;
    let g = groups.get(p.group);
    if (!g) {
      g = { group: p.group, label: p.group_label || p.group, params: [] };
      groups.set(p.group, g);
    }
    g.params.push(p);
  }
  return [...groups.values()];
}

/** Counts parameters per level, so the filter can say how many each level would add. */
export function countByLevel(params: readonly ParamSpec[]): Record<string, number> {
  const out: Record<string, number> = {};
  for (const p of params) out[p.level] = (out[p.level] ?? 0) + 1;
  return out;
}

/** Value equality across the JSON types a parameter can take ("auto" vs 4096, 8 vs "8"). */
export function sameValue(a: ParamValue | undefined, b: ParamValue | undefined): boolean {
  if (a === b) return true;
  if (a === undefined || b === undefined) return false;
  if (typeof a === 'number' && typeof b === 'string') return b.trim() !== '' && Number(b) === a;
  if (typeof a === 'string' && typeof b === 'number') return a.trim() !== '' && Number(a) === b;
  return false;
}

export function isDefault(spec: ParamSpec, value: ParamValue | undefined): boolean {
  return value === undefined || sameValue(spec.default, value);
}
