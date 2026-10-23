// Plan helpers: map decisions to parameter keys for the settings source badges, and turn the plan
// into what the Plan page draws. Nothing here names a device, a system or a knob, and nothing here
// words a code: every word comes from the server (server/plan_view.h), and a code (`status`,
// `uses`, `step`) only picks a colour. A server that sends no word gets the code shown as it is.
import { displayValue } from './coerce';
import { commandLine } from './format';
import { sameValue, specialLabel } from './schema';
import type { Config, ParamSpec, ParamValue, PlanDecision, PlanDevice, PlanMeasured, PlanMemoryPool } from './types';

/**
 * `config.plan` is documented as "the last applied plan's decisions". Accept either a bare array
 * of decisions or an object carrying `decisions`, so both readings of the contract work.
 */
export function planDecisions(plan: Config['plan'] | undefined): PlanDecision[] {
  if (!plan) return [];
  if (Array.isArray(plan)) return plan;
  return Array.isArray(plan.decisions) ? plan.decisions : [];
}

export function decisionsByKnob(decisions: readonly PlanDecision[]): Map<string, PlanDecision> {
  const out = new Map<string, PlanDecision>();
  for (const d of decisions) out.set(d.knob, d);
  return out;
}

export interface DecisionGroups {
  set: PlanDecision[];
  kept: PlanDecision[];
  notes: PlanDecision[];
}

/** The three groups a plan is read in, as the server says (server/plan_view.cpp decides them). The UI
 *  ships with its own server, so a decision without a group is one it puts with the settings set. */
export function groupDecisions(decisions: readonly PlanDecision[]): DecisionGroups {
  const out: DecisionGroups = { set: [], kept: [], notes: [] };
  for (const d of decisions) {
    const g = d.group ?? 'set';
    (g === 'notes' ? out.notes : g === 'kept' ? out.kept : out.set).push(d);
  }
  return out;
}

/** The words a decision is shown under: the settings label, else the knob as the plan names it. */
export function decisionLabel(d: PlanDecision): string {
  return d.label || d.knob;
}

export interface MemorySegment {
  label: string;
  bytes: number;
  /** Share of the bar, 0..1. */
  frac: number;
  locked: boolean;
  /** Position among the held rows, which picks the colour; -1 for the free part. */
  slot: number;
}

/**
 * A pool as one bar: every row held for the whole run, then what is left free, against the larger
 * of the cap and what is held (a ledger that does not close still draws). Rows held in turn with
 * the cache are not on the bar: they are the cache's own bytes, lent during a prompt.
 */
export function memorySegments(pool: PlanMemoryPool): MemorySegment[] {
  const held = pool.rows.filter((r) => !r.in_turn);
  const sum = held.reduce((a, r) => a + r.bytes, 0);
  const whole = Math.max(pool.cap_bytes, sum);
  if (whole <= 0) return [];
  const out: MemorySegment[] = held.map((r, i) => ({ label: r.label ?? r.name, bytes: r.bytes, frac: r.bytes / whole, locked: r.locked, slot: i }));
  const free = whole - sum;
  if (free > 0) out.push({ label: 'Free', bytes: free, frac: free / whole, locked: false, slot: -1 });
  return out;
}

/** A badge's colour, from a device's role alone: green when it works here, red when it was refused. */
export function deviceTone(d: PlanDevice): 'ok' | 'err' | '' {
  if (d.status === 'refused' || d.status === 'unusable') return 'err';
  return d.status === 'used' ? 'ok' : '';
}

/** Bars are scaled to the fastest device measured, so 1x on one machine and 40x on another read alike. */
export function rateScale(devices: readonly PlanDevice[]): number {
  return devices.reduce((m, d) => Math.max(m, d.prompt_ratio), 1);
}

export interface SpeedCheck {
  predictedTokS: number | null;
  measuredTokS: number | null;
  /** Measured against predicted, as bmoe-cli prints it: positive when the run beat the plan. */
  deltaPct: number | null;
  predictedHitPct: number | null;
  measuredHitPct: number | null;
}

export function speedCheck(predicted: { s_per_token?: number; cache_hit_pct?: number } | undefined, measured: PlanMeasured | undefined): SpeedCheck {
  const ps = predicted?.s_per_token ?? 0;
  const ms = measured?.s_per_token ?? 0;
  return {
    predictedTokS: ps > 0 ? 1 / ps : null,
    measuredTokS: ms > 0 ? 1 / ms : null,
    deltaPct: ps > 0 && ms > 0 ? 100 * (ps / ms - 1) : null,
    predictedHitPct: predicted?.cache_hit_pct ?? null,
    measuredHitPct: measured && measured.cache_hit_pct >= 0 ? measured.cache_hit_pct : null,
  };
}

export interface AppliedChange {
  key: string;
  /** The row's settings label; the key where the table has no row for it. */
  label: string;
  from: string;
  to: string;
}

function shown(spec: ParamSpec | undefined, v: ParamValue | undefined): string {
  if (v === undefined) return '';
  const named = spec ? specialLabel(spec, v) : undefined;
  const choice = spec?.choices?.find((c) => c.value === String(v))?.label;
  const text = named ?? choice ?? displayValue(v);
  return spec?.unit && typeof v === 'number' && !named ? `${text} ${spec.unit}` : text;
}

/** What applying a plan changed, row by row in table order, worded from the table. */
export function appliedChanges(
  before: Record<string, ParamValue>,
  after: Record<string, ParamValue>,
  params: readonly ParamSpec[],
): AppliedChange[] {
  const out: AppliedChange[] = [];
  const known = new Set(params.map((p) => p.key));
  const keys = [...params.map((p) => p.key), ...Object.keys(after).filter((k) => !known.has(k))];
  for (const key of keys) {
    if (!(key in after) || sameValue(before[key], after[key])) continue;
    const spec = params.find((p) => p.key === key);
    out.push({ key, label: spec?.label || key, from: shown(spec, before[key]), to: shown(spec, after[key]) });
  }
  return out;
}

/** What "Copy diagnostic" puts on the clipboard: the plan's command and the planner's whole account. */
export function diagnosticText(args: readonly string[], explain: string): string {
  const cmd = args.length ? commandLine(args) : '(no arguments: the plan declines streaming)';
  return explain.trim() ? `${cmd}\n\n${explain.trim()}\n` : `${cmd}\n`;
}
