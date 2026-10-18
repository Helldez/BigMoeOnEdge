// Schema helpers: everything the settings form knows about parameters comes from here, and all
// of it is derived from the `GET /api/params` rows. No parameter is named in this module.
import type { ParamLevel, ParamSpec, ParamValue } from './types';

/** Every level, in order of how much a person needs to know before touching a row. */
export const ALL_LEVELS: readonly ParamLevel[] = ['basic', 'advanced', 'experimental', 'debug'];
/** The levels beyond basic, in display order. */
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

/**
 * The rows behind the Advanced fold: everything but basic, grouped like the form above it. A basic
 * row is shown once, in its group, never again under Advanced.
 */
export function advancedGroups(params: readonly ParamSpec[]): ParamGroup[] {
  // Grouped over the whole table first, so the groups keep the order of the form above.
  return groupParams(params, new Set(OPTIONAL_LEVELS))
    .map((g) => ({ ...g, params: g.params.filter((p) => p.level !== 'basic') }))
    .filter((g) => g.params.length > 0);
}

/** The name of a value that means something other than its number ("off" for 0), or undefined. */
export function specialLabel(spec: ParamSpec, value: ParamValue | undefined): string | undefined {
  return spec.special?.find((s) => sameValue(s.value, value))?.label;
}

/** What a person usually sets, in words: "usually 2 to 8". The unit sits by the field already. */
export function typicalText(spec: ParamSpec): string {
  if (!spec.typical) return '';
  const { min, max } = spec.typical;
  return `usually ${min === max ? `${min}` : `${min} to ${max}`}`;
}

/**
 * The help split into its first sentence, always shown, and the rest, shown on demand. A sentence
 * ends at a full stop followed by a space and a capital or a digit, outside parentheses (where
 * "e.g. HTP0" lives), once the lead is long enough to stand alone.
 */
export function helpParts(help: string): { lead: string; rest: string } {
  const text = help.trim();
  const re = /[.!?]\s+(?=[A-Z0-9])/g;
  for (let m = re.exec(text); m; m = re.exec(text)) {
    const before = text.slice(0, m.index);
    const depth = (before.match(/\(/g)?.length ?? 0) - (before.match(/\)/g)?.length ?? 0);
    const end = m.index + 1;
    if (depth <= 0 && end >= 24) return { lead: text.slice(0, end), rest: text.slice(m.index + m[0].length) };
  }
  return { lead: text, rest: '' };
}

/** The plan's value for a row when it differs from the one in force, for the form to offer. */
export function planSuggestion(value: ParamValue | undefined, planned: string | undefined): string | undefined {
  if (planned === undefined || planned === '') return undefined;
  return sameValue(value, planned) || String(value) === planned ? undefined : planned;
}
