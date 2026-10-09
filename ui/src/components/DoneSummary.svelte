<script lang="ts">
  import { formatMib, formatNum } from '../lib/format';
  import type { DoneSummary } from '../lib/types';

  let { done }: { done: DoneSummary } = $props();
  const ms = (s: number) => formatNum(s * 1000, 1);
</script>

<dl class="summary">
  <dt>Tokens</dt>
  <dd>{done.tokens}{done.cancelled ? ' (cancelled)' : ''}</dd>
  <dt>Decode</dt>
  <dd>{formatNum(done.tok_s, 2)} tok/s</dd>
  <dt>Prefill</dt>
  <dd>{formatNum(done.prefill_s, 2)} s, {formatNum(done.prefill_tps, 1)} tok/s ({done.n_prompt} tokens)</dd>
  <dt>TTFT</dt>
  <dd>{formatNum(done.ttft_s, 2)} s</dd>
  <dt>Context</dt>
  <dd>{done.n_past} tokens</dd>
  <dt>Cache hit</dt>
  <dd>{done.cache_hit_pct < 0 ? 'no cache' : `${formatNum(done.cache_hit_pct, 1)}%`}</dd>
  <dt>Cache</dt>
  <dd>{formatMib(done.cache_resident_mib)} of {formatMib(done.cache_budget_mib)}</dd>
  <dt>Read</dt>
  <dd>{formatMib(done.read_mib)}</dd>
  <dt>Per token</dt>
  <dd>
    io {ms(done.io_s_tok)} ms, compute {ms(done.compute_s_tok)} ms, stall {ms(done.stall_s_tok)} ms, mgmt {ms(
      done.mgmt_s_tok,
    )} ms
  </dd>
</dl>

<style>
  .summary {
    display: grid;
    grid-template-columns: max-content 1fr;
    gap: 0.15rem 0.75rem;
    margin: 0;
    font-size: 0.85rem;
  }
  dt {
    color: var(--muted);
  }
  dd {
    margin: 0;
    font-variant-numeric: tabular-nums;
  }
</style>
