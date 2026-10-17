// The one-line figure under a finished answer, from the turn's own summary: the decode rate and
// length, the prefill rate with the tokens it read and the time it took, the context used, the
// cache hit rate, and under speculation the real rate and the share of guesses kept. Each rate
// names its phase, since the two differ by an order of magnitude and a bare "tok/s" reads as
// either. The same line the Android app writes (docs/screens.md), so a turn reads alike on both.
import type { DoneSummary } from './types';

export function turnMetrics(s: DoneSummary, nCtx: number, cancelled: boolean): string {
  const parts: string[] = [`decode ${s.tok_s.toFixed(1)} tok/s (${s.tokens} tok)`];
  if (s.prefill_s > 0) {
    // The tokens this turn prefilled: with the conversation's leading tokens reused from the KV
    // cache, that is the new message, not the whole history.
    const tps = s.prefill_tps > 0 ? s.prefill_tps : s.n_prompt > 0 ? s.n_prompt / s.prefill_s : 0;
    const read = s.n_prompt >= 0 ? `${s.n_prompt} tok, ` : '';
    parts.push(`prefill ${tps.toFixed(1)} tok/s (${read}${s.prefill_s.toFixed(1)} s)`);
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
