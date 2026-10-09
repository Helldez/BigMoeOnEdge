// Plan helpers: map decisions to parameter keys for the settings source badges.
import type { Config, PlanDecision } from './types';

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
