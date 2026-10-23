// Schema helpers: everything the settings form knows about parameters comes from here, and all
// of it is derived from the `GET /api/params` rows. No parameter is named in this module.
import type { ParamLevel, ParamSpec, ParamValue, PlanDecision } from './types';

/** Every level, in order of how much a person needs to know before touching a row. */
export const ALL_LEVELS: readonly ParamLevel[] = ['basic', 'advanced', 'experimental', 'debug'];

/** A level's place in ALL_LEVELS; a level this UI does not know yet sorts last. */
export function levelRank(level: string): number {
  const i = (ALL_LEVELS as readonly string[]).indexOf(level);
  return i < 0 ? ALL_LEVELS.length : i;
}

/** The rows of one level that a card folds away. */
export interface LevelRows {
  level: string;
  params: ParamSpec[];
}

/** One settings card: a group of the table, its basic rows open and the rest folded by level. */
export interface SettingsCard {
  group: string;
  label: string;
  help: string;
  /** Shown, not edited: a front-end sets it with a control of its own. */
  readOnly: boolean;
  basic: ParamSpec[];
  more: LevelRows[];
  /** Rows of this group the level filter leaves out. */
  hidden: number;
}

export interface CardFilter {
  /** The deepest level shown: every row at or above it. */
  upTo: string;
  /** Words to look for; when set, the level filter is ignored and only matching rows are kept. */
  query?: string;
  /** Rows shown whatever the filter (a value someone or the plan set must never hide). */
  pinned?: ReadonlySet<string>;
}

function matches(p: ParamSpec, words: string[]): boolean {
  const hay = `${p.label} ${p.help} ${p.flag} ${p.key} ${p.group_label}`.toLowerCase();
  return words.every((w) => hay.includes(w));
}

/**
 * The settings form: one card per group, in the table's group order (`group_order`, else the order
 * groups first appear), rows in table order. Nothing here names a group or a parameter.
 */
export function settingsCards(params: readonly ParamSpec[], filter: CardFilter): SettingsCard[] {
  const words = (filter.query ?? '').toLowerCase().split(/\s+/).filter(Boolean);
  const upTo = levelRank(filter.upTo);
  const cards = new Map<string, { card: SettingsCard; order: number; first: number }>();
  params.forEach((p, i) => {
    let entry = cards.get(p.group);
    if (!entry) {
      entry = {
        card: {
          group: p.group,
          label: p.group_label || p.group,
          help: p.group_help ?? '',
          readOnly: p.group_read_only === true,
          basic: [],
          more: [],
          hidden: 0,
        },
        order: p.group_order ?? Number.MAX_SAFE_INTEGER,
        first: i,
      };
      cards.set(p.group, entry);
    }
    const c = entry.card;
    const shown = words.length
      ? matches(p, words)
      : p.level === 'basic' || levelRank(p.level) <= upTo || (filter.pinned?.has(p.key) ?? false);
    if (!shown) {
      if (!words.length) c.hidden++;
      return;
    }
    if (p.level === 'basic') {
      c.basic.push(p);
      return;
    }
    let lv = c.more.find((m) => m.level === p.level);
    if (!lv) {
      lv = { level: p.level, params: [] };
      c.more.push(lv);
    }
    lv.params.push(p);
  });
  return [...cards.values()]
    .filter((e) => e.card.basic.length + e.card.more.length > 0)
    .sort((a, b) => a.order - b.order || a.first - b.first)
    .map((e) => ({ ...e.card, more: [...e.card.more].sort((a, b) => levelRank(a.level) - levelRank(b.level)) }));
}

/** The level a row of the table sits at, or undefined for a key the table does not have. */
export function levelOf(params: readonly ParamSpec[], key: string): string | undefined {
  return params.find((p) => p.key === key)?.level;
}

/** How many rows each level holds, for the filter's counts. */
export function levelCounts(params: readonly ParamSpec[]): Map<string, number> {
  const out = new Map<string, number>();
  for (const p of params) out.set(p.level, (out.get(p.level) ?? 0) + 1);
  return out;
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

