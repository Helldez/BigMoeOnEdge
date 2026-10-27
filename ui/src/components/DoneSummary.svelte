<script lang="ts">
  // The last answer's account, from its `done` event, in plain labels.
  import { formatMib, formatNum } from '../lib/format';
  import type { DoneSummary } from '../lib/types';
  import { t } from '../stores/prefs.svelte';

  let { done }: { done: DoneSummary } = $props();
  const ms = (s: number) => formatNum(s * 1000, 1);
</script>

<dl class="summary figure">
  <dt>{t('done.tokens')}</dt>
  <dd>{done.tokens}{done.cancelled ? ` (${t('done.stopped')})` : ''}</dd>
  <dt title={t('live.speed.hint')}>{t('done.speed')}</dt>
  <dd>{formatNum(done.tok_s, 2)} tok/s</dd>
  <dt title={t('done.reading.hint')}>{t('done.reading')}</dt>
  <dd>{formatNum(done.prefill_s, 2)} s · {formatNum(done.prefill_tps, 1)} tok/s ({done.n_prompt} tok)</dd>
  <dt>{t('done.first')}</dt>
  <dd>{formatNum(done.ttft_s, 2)} s</dd>
  <dt title={t('done.context.hint')}>{t('done.context')}</dt>
  <dd>{done.n_past} tok</dd>
  <dt title={t('live.cache.hint')}>{t('done.cache')}</dt>
  <dd>{done.cache_hit_pct < 0 ? t('metrics.cache.none') : `${formatNum(done.cache_hit_pct, 1)}%`} · {formatMib(done.cache_resident_mib)} / {formatMib(done.cache_budget_mib)}</dd>
  <dt>{t('done.read')}</dt>
  <dd>{formatMib(done.read_mib)}</dd>
  <dt>{t('done.perToken')}</dt>
  <dd>{t('done.perToken.value', { io: ms(done.io_s_tok), compute: ms(done.compute_s_tok), stall: ms(done.stall_s_tok), mgmt: ms(done.mgmt_s_tok) })}</dd>
</dl>

<style>
  .summary {
    display: grid;
    grid-template-columns: max-content 1fr;
    gap: var(--s1) var(--s4);
    margin: 0;
    font-size: 0.875rem;
  }
  dt {
    color: var(--muted);
  }
  dt[title] {
    cursor: help;
  }
  dd {
    margin: 0;
  }
</style>
