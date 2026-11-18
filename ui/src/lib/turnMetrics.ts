// The figures of a finished answer, from the turn's own summary, in the words every other screen
// uses for the same quantity. Only one figure is a set of percentages, and it adds up to 100: where
// each token's time went. What was found in memory is a different quantity, so it is said as a
// count ("72 of every 100 parts"), never as a percentage beside the time shares a person would try
// to add up. The answer speed carries the rate the plan expects.
import type { DoneSummary } from './types';
import { formatNum } from './format';
import { t } from './text';

export interface TurnFigure {
  /** Stable id, for keys and tests. */
  id: 'speed' | 'time' | 'cache' | 'prompt' | 'context' | 'drafts' | 'stopped';
  label: string;
  value: string;
  /** What the figure measures, in plain words: shown when the person opens the figures. */
  hint: string;
  /** In the one-line summary under the answer, or only in the opened list. */
  lead: boolean;
}

export interface TimeShare {
  key: 'compute' | 'disk' | 'mgmt' | 'other';
  pct: number;
}

/**
 * Where each token's time went, as whole percentages that add up to exactly 100 (largest
 * remainder): computing, waiting for the disk, managing the cache, and what no measured term
 * claims. Parts that round to nothing are left out. Empty when the turn has no rate.
 */
export function timeShares(s: DoneSummary): TimeShare[] {
  if (!(s.tok_s > 0)) return [];
  const wall = 1 / s.tok_s;
  const compute = Math.max(0, s.compute_s_tok);
  const disk = Math.max(0, s.flash_wait_s_tok ?? 0);
  const mgmt = Math.max(0, s.mgmt_s_tok);
  const other = Math.max(0, wall - compute - disk - mgmt);
  const parts: [TimeShare['key'], number][] = [['compute', compute], ['disk', disk], ['mgmt', mgmt], ['other', other]];
  const total = parts.reduce((a, [, v]) => a + v, 0);
  if (total <= 0) return [];
  const exact = parts.map(([, v]) => (100 * v) / total);
  const pct = exact.map((x) => Math.floor(x));
  let left = 100 - pct.reduce((a, b) => a + b, 0);
  for (const i of exact.map((x, i) => ({ i, r: x - Math.floor(x) })).sort((a, b) => b.r - a.r).map((o) => o.i)) {
    if (left <= 0) break;
    pct[i]++;
    left--;
  }
  return parts.map(([key], i) => ({ key, pct: pct[i] })).filter((p) => p.pct > 0);
}

export function turnFigures(s: DoneSummary, nCtx: number, expectedTokS: number | null): TurnFigure[] {
  const out: TurnFigure[] = [];
  const expected = expectedTokS !== null && expectedTokS > 0 ? t('figure.speed.expected', { rate: formatNum(expectedTokS, 1) }) : '';
  out.push({
    id: 'speed',
    label: t('figure.speed'),
    value: t('figure.rate', { rate: formatNum(s.tok_s, 1) }) + expected,
    hint: t('figure.speed.hint'),
    lead: true,
  });
  const shares = timeShares(s);
  if (shares.length)
    out.push({
      id: 'time',
      label: t('figure.time'),
      value: shares.map((p) => t(`figure.time.${p.key}`, { pct: p.pct })).join(' · '),
      hint: t('figure.time.hint'),
      lead: false,
    });
  if (s.cache_hit_pct >= 0)
    out.push({
      id: 'cache',
      label: t('figure.cache'),
      value: t('figure.cache.value', { n: Math.round(s.cache_hit_pct) }),
      hint: t('figure.cache.hint'),
      lead: false,
    });
  if (s.prefill_s > 0) {
    // The tokens this turn read: with the conversation's leading tokens reused from the KV cache,
    // that is the new message, not the whole history.
    const tps = s.prefill_tps > 0 ? s.prefill_tps : s.n_prompt > 0 ? s.n_prompt / s.prefill_s : 0;
    out.push({
      id: 'prompt',
      label: t('figure.prompt'),
      value: t('figure.prompt.value', { rate: formatNum(tps, 1), n: Math.max(0, s.n_prompt), s: formatNum(s.prefill_s, 1) }),
      hint: t('figure.prompt.hint'),
      lead: false,
    });
  }
  if (s.n_past >= 0 && nCtx > 0)
    out.push({
      id: 'context',
      label: t('figure.context'),
      value: t('figure.context.value', { used: s.n_past.toLocaleString('en'), total: nCtx.toLocaleString('en') }),
      hint: t('figure.context.hint'),
      lead: false,
    });
  const sp = s as Partial<{ mtp_decodes: number; mtp_drafted: number; mtp_accepted: number }>;
  if ((sp.mtp_decodes ?? 0) > 0 && (sp.mtp_drafted ?? 0) > 0)
    out.push({
      id: 'drafts',
      label: t('figure.drafts'),
      value: t('figure.drafts.value', { kept: sp.mtp_accepted ?? 0, drafted: sp.mtp_drafted ?? 0 }),
      hint: t('figure.drafts.hint'),
      lead: false,
    });
  if (s.cancelled) out.push({ id: 'stopped', label: t('figure.stopped'), value: '', hint: t('figure.stopped.hint'), lead: true });
  return out;
}
