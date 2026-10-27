<script lang="ts">
  // The rate and where the time goes, one line, while an answer streams and after; the full card
  // is on Metrics. Each figure says what it is, with a tooltip for the words a person may not know.
  import { formatNum } from '../../lib/format';
  import { server } from '../../stores/server.svelte';
  import { t } from '../../stores/prefs.svelte';

  let m = $derived(server.metrics);
  let last = $derived(m.last);
  let done = $derived(server.lastDone);
  // The flash term is the one the server chose as a share of the wall (the stall under overlap,
  // the blocking read in serial): the summed lane time can exceed the wall and is not a share of it.
  let parts = $derived.by(() => {
    const out: { text: string; hint: string }[] = [];
    let rate: number | null = null;
    let compute: number | null = null;
    let flash: number | null = null;
    let cache = -1;
    if (done) {
      rate = done.tok_s;
      const wall = done.tok_s > 0 ? 1 / done.tok_s : 0;
      if (wall > 0) {
        compute = (100 * done.compute_s_tok) / wall;
        flash = (100 * (done.flash_wait_s_tok ?? 0)) / wall;
      }
      cache = done.cache_hit_pct;
    } else if (last) {
      rate = m.tokS;
      if (last.wall_ms > 0) {
        compute = (100 * last.compute_ms) / last.wall_ms;
        flash = (100 * (last.flash_wait_ms ?? 0)) / last.wall_ms;
      }
      cache = last.cache_hit_pct;
    } else return out;
    out.push({ text: t('live.speed', { rate: formatNum(rate, 1) }), hint: t('live.speed.hint') });
    if (compute !== null) out.push({ text: t('live.compute', { pct: Math.round(compute) }), hint: t('live.compute.hint') });
    if (flash !== null) out.push({ text: t('live.disk', { pct: Math.round(flash) }), hint: t('live.disk.hint') });
    if (cache >= 0) out.push({ text: t('live.cache', { pct: Math.round(cache) }), hint: t('live.cache.hint') });
    return out;
  });
</script>

{#if parts.length}
  <p class="live muted figure" aria-live="off">
    {#each parts as p, i (i)}{#if i > 0}<span aria-hidden="true">{" · "}</span>{/if}<span title={p.hint}>{p.text}</span>{/each}
  </p>
{/if}

<style>
  .live {
    margin: 0;
    padding: 0 var(--s2);
    font-size: 0.75rem;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .live span[title] {
    cursor: help;
    text-decoration: underline dotted;
    text-underline-offset: 3px;
  }
</style>
