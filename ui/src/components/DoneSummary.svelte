<script lang="ts">
  // The last answer's account, from its `done` event, in the words the figures under an answer use.
  // Where its time went is the split above this card, so it is not repeated here in other terms.
  import { formatMib, formatNum } from '../lib/format';
  import type { DoneSummary } from '../lib/types';
  import { t } from '../lib/text';

  let { done, nCtx = 0 }: { done: DoneSummary; nCtx?: number } = $props();
  // The cache's own account, which a field report could only read from the command line: how big it
  // is against what it may hold, what it had to read again, and the disk's share of a token in time.
  let cached = $derived(done.cache_budget_mib > 0);
  let rereads = $derived(cached && done.tokens > 0 && typeof done.cache_rereads === 'number' ? done.cache_rereads / done.tokens : null);
  let waitMs = $derived(done.read_mib > 0 && typeof done.flash_wait_s_tok === 'number' && done.flash_wait_s_tok >= 0 ? done.flash_wait_s_tok * 1000 : null);
</script>

<dl class="summary figure">
  <dt>{t('done.tokens')}</dt>
  <dd>{done.tokens}{done.cancelled ? ` (${t('done.stopped')})` : ''}</dd>
  <dt>{t('figure.speed')}</dt>
  <dd>{t('figure.rate', { rate: formatNum(done.tok_s, 1) })}</dd>
  <dt>{t('figure.prompt')}</dt>
  <dd>{t('figure.prompt.value', { rate: formatNum(done.prefill_tps, 1), n: Math.max(0, done.n_prompt), s: formatNum(done.prefill_s, 1) })}</dd>
  <dt>{t('done.first')}</dt>
  <dd>{formatNum(done.ttft_s, 2)} s</dd>
  <dt>{t('figure.context')}</dt>
  <dd>{nCtx > 0 ? t('figure.context.value', { used: done.n_past.toLocaleString('en'), total: nCtx.toLocaleString('en') }) : done.n_past.toLocaleString('en')}</dd>
  <dt>{t('figure.cache')}</dt>
  <dd>{done.cache_hit_pct < 0 ? t('metrics.cache.none') : t('figure.cache.value', { n: Math.round(done.cache_hit_pct) })}</dd>
  <dt>{t('done.read')}</dt>
  <dd>{formatMib(done.read_mib)}</dd>
  {#if cached}
    <dt>{t('done.cacheSize')}</dt>
    <dd>{t('done.cacheSize.value', { held: formatMib(done.cache_resident_mib), budget: formatMib(done.cache_budget_mib) })}</dd>
  {/if}
  {#if rereads !== null}
    <dt>{t('done.rereads')}</dt>
    <dd>{t('done.rereads.value', { n: formatNum(rereads, 1) })}</dd>
  {/if}
  {#if waitMs !== null}
    <dt>{t('figure.disk')}</dt>
    <dd>{t('done.wait.value', { ms: formatNum(waitMs, 0) })}</dd>
  {/if}
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
  dd {
    margin: 0;
  }
</style>
