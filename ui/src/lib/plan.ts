// Plan helpers: map decisions to parameter keys for the settings source badges, and turn the plan
// into what the Plan page draws. Nothing here names a device, a system or a knob: every word comes
// from the plan, and the shapes (pools, rows, devices, stages) are lists of whatever it holds.
import type { Config, PlanDecision, PlanDevice, PlanMeasured, PlanMemoryPool } from './types';

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

/** The three groups a plan is read in. A server without `group` gets the nearest split it can say. */
export function groupDecisions(decisions: readonly PlanDecision[]): DecisionGroups {
  const out: DecisionGroups = { set: [], kept: [], notes: [] };
  for (const d of decisions) {
    const g = d.group ?? (d.knob === 'cost' ? 'notes' : d.source === 'operator' || d.source === 'unprobed' ? 'kept' : 'set');
    (g === 'notes' ? out.notes : g === 'kept' ? out.kept : out.set).push(d);
  }
  return out;
}

/** The words a decision is shown under: the settings label, else the knob as the plan names it. */
export function decisionLabel(d: PlanDecision): string {
  return d.label || d.knob;
}

/** "expert cache" -> "Expert cache": the plan's own words, sentence case. */
export function sentenceCase(s: string): string {
  return s ? s[0].toUpperCase() + s.slice(1) : s;
}

export interface MemorySegment {
  name: string;
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
  const out: MemorySegment[] = held.map((r, i) => ({ name: r.name, bytes: r.bytes, frac: r.bytes / whole, locked: r.locked, slot: i }));
  const free = whole - sum;
  if (free > 0) out.push({ name: 'free', bytes: free, frac: free / whole, locked: false, slot: -1 });
  return out;
}

export interface DeviceBadge {
  text: string;
  tone: 'ok' | 'err' | '';
}

const USE_WORDS: Record<string, string> = { prompt: 'prompt', tokens: 'tokens', layers: 'layers' };

/** The badge a device row carries, from its role alone: what it does, or why it does nothing. */
export function deviceBadge(d: PlanDevice): DeviceBadge {
  if (d.status === 'refused') return { text: 'Wrong results', tone: 'err' };
  if (d.status === 'unusable') return { text: 'Unusable', tone: 'err' };
  if (d.status !== 'used' || d.uses.length === 0) return { text: 'Not used', tone: '' };
  const words = d.uses.map((u) => USE_WORDS[u] ?? u);
  const text = words.length > 1 ? `${words.slice(0, -1).join(', ')} and ${words[words.length - 1]}` : words[0];
  return { text: sentenceCase(text), tone: 'ok' };
}

/** Bars are scaled to the fastest device measured, so 1x on one machine and 40x on another read alike. */
export function rateScale(devices: readonly PlanDevice[]): number {
  return devices.reduce((m, d) => Math.max(m, d.prompt_ratio), 1);
}

export function ratioText(r: number): string {
  if (!(r > 0)) return 'not measured';
  return r >= 10 ? `${r.toFixed(0)}x` : `${r.toFixed(1)}x`;
}

/** Why the list holds no device beyond the host: three answers that must not read as one. */
export function devicesNoneText(code: string | undefined): string {
  switch (code) {
    case 'build':
      return 'No other device: this build carries no backend for one.';
    case 'machine':
      return 'No other device: none of the backends this build carries found one here.';
    case 'not_probed':
      return 'No other device was looked for.';
    default:
      return '';
  }
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

/** The measurement's stages in words; a stage this UI does not know is shown as the server names it. */
const STEP_WORDS: Record<string, string> = {
  devices: 'Devices',
  model: 'Model',
  compute: 'Compute',
  storage: 'Storage',
  memory: 'Memory',
  placement: 'Placement',
  rules: 'Decisions',
};

export function stepLabel(step: string): string {
  return STEP_WORDS[step] ?? sentenceCase(step);
}
