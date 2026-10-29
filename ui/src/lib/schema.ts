// Schema helpers: everything the settings form knows about parameters comes from here, and all
// of it is derived from the `GET /api/params` rows. No parameter is named in this module.
import type { ParamSpec, ParamValue, PlanDecision } from './types';

/** Whether a settings form offers a row: the table says so (`form`); a server too old to say, its basic rows. */
export function inForm(p: ParamSpec): boolean {
  return p.form ?? p.level === 'basic';
}

/** One settings card: a group of the table and the rows of it a form offers. */
export interface SettingsCard {
  group: string;
  label: string;
  help: string;
  /** Shown, not edited: a front-end sets it with a control of its own. */
  readOnly: boolean;
  params: ParamSpec[];
}

/**
 * The settings form: one card per group, in the table's group order (`group_order`, else the order
 * groups first appear), rows in table order: the rows the table puts in forms, less the ones the
 * person set, which `setByYou` lists instead (each row appears once). Nothing here names a group or
 * a parameter.
 */
export function settingsCards(params: readonly ParamSpec[], userKeys?: ReadonlySet<string>): SettingsCard[] {
  const cards = new Map<string, { card: SettingsCard; order: number; first: number }>();
  params.forEach((p, i) => {
    if (!inForm(p) || (p.group_read_only !== true && (userKeys?.has(p.key) ?? false))) return;
    let entry = cards.get(p.group);
    if (!entry) {
      entry = {
        card: {
          group: p.group,
          label: p.group_label || p.group,
          help: p.group_help ?? '',
          readOnly: p.group_read_only === true,
          params: [],
        },
        order: p.group_order ?? Number.MAX_SAFE_INTEGER,
        first: i,
      };
      cards.set(p.group, entry);
    }
    entry.card.params.push(p);
  });
  return [...cards.values()].sort((a, b) => a.order - b.order || a.first - b.first).map((e) => e.card);
}

/**
 * Every row the person set, in or out of the form, in table order: what "Set by you" lists. Not a
 * read-only group's (the model, chosen on Models), which Settings cannot reset and keeps in its card.
 */
export function setByYou(params: readonly ParamSpec[], userKeys: ReadonlySet<string>): ParamSpec[] {
  return params.filter((p) => userKeys.has(p.key) && p.group_read_only !== true);
}

/**
 * What "reset all my values" removes: the person's own values, never a key the server was launched
 * with (the operator's choice for this run) and never a row of a read-only group, which a client
 * sets with a control of its own (the model a load chose) and the form could not set back.
 */
export function resettableKeys(
  userKeys: readonly string[],
  launchKeys: readonly string[],
  params: readonly ParamSpec[],
): string[] {
  const launch = new Set(launchKeys);
  const readOnly = new Set(params.filter((p) => p.group_read_only).map((p) => p.key));
  return userKeys.filter((k) => !launch.has(k) && !readOnly.has(k));
}

/** Where a value came from, in the one chip a row carries. */
export type ValueSource = 'you' | 'plan' | 'default';

/** The person's value wins; else a plan that decided the row; else the server's default. */
export function valueSource(userSet: boolean, decision: PlanDecision | undefined): ValueSource {
  if (userSet) return 'you';
  if (decision && (decision.group ?? 'set') === 'set') return 'plan';
  return 'default';
}

/** True when the plan decided this row and the value in force is not the one it decided. */
export function differsFromPlan(value: ParamValue | undefined, decision: PlanDecision | undefined): boolean {
  if (!decision || (decision.group ?? 'set') !== 'set' || value === undefined) return false;
  return !(sameValue(value, decision.value) || String(value) === decision.value);
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

