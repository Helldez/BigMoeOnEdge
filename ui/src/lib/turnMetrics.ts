// The one-line figure under a finished answer, from the turn's own summary: rate and length, the
// prompt it cost, the context used, the cache hit rate, and under speculation the real rate and
// the share of guesses kept. The same line the Android app writes, so a turn reads alike on both.
import type { DoneSummary } from './types';

export function turnMetrics(s: DoneSummary, nCtx: number, cancelled: boolean): string {
  const parts: string[] = [`${s.tok_s.toFixed(1)} tok/s`, `${s.tokens} tok`];
  if (s.prefill_s > 0) {
    parts.push(`prefill ${s.prefill_s.toFixed(1)}s` + (s.n_prompt >= 0 ? ` (${s.n_prompt} tok)` : ''));
  }
  if (s.n_past >= 0 && nCtx > 0) parts.push(`ctx ${s.n_past}/${nCtx}`);
  if (s.cache_hit_pct >= 0) parts.push(`cache ${Math.round(s.cache_hit_pct)}%`);
  const sp = s as Partial<{ mtp_decodes: number; mtp_drafted: number; mtp_accepted: number; loop_overhead_s_tok: number }>;
  if ((sp.mtp_decodes ?? 0) > 0 && (sp.mtp_drafted ?? 0) > 0) {
    if (s.tok_s > 0) parts.push(`${(1 / (1 / s.tok_s + (sp.loop_overhead_s_tok ?? 0))).toFixed(1)} real`);
    parts.push(`${Math.round((100 * (sp.mtp_accepted ?? 0)) / (sp.mtp_drafted ?? 1))}% kept`);
  }
  if (cancelled) parts.push('stopped');
  return parts.join(' · ');
}
