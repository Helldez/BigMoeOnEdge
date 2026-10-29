<script lang="ts">
  // The live figures: speed, disk reads, memory and the expert cache, each with its recent history,
  // then where a token's time goes and the last answer's account. Every figure comes from the
  // `token` and `done` events; a dash is "not measured yet", never a zero.
  import DoneSummary from './DoneSummary.svelte';
  import Sparkline from './Sparkline.svelte';
  import { formatMib, formatNum, formatPct } from '../lib/format';
  import { server } from '../stores/server.svelte';
  import { t } from '../lib/text';

  let m = $derived(server.metrics);
  let last = $derived(m.last);
  let done = $derived(server.lastDone);
  let memFrac = $derived(last && last.rss_mib > 0 && last.mem_available_mib > 0 ? last.rss_mib / (last.rss_mib + last.mem_available_mib) : null);

  // A token's wall time in the four terms the engine measures or leaves unclaimed (telemetry.md).
  let split = $derived.by(() => {
    if (!done || done.tok_s <= 0) return null;
    const wall = 1 / done.tok_s;
    const compute = Math.max(0, done.compute_s_tok);
    const disk = Math.max(0, done.flash_wait_s_tok ?? 0);
    const mgmt = Math.max(0, done.mgmt_s_tok);
    const other = Math.max(0, wall - compute - disk - mgmt);
    const total = compute + disk + mgmt + other;
    if (total <= 0) return null;
    return [
      { key: 'compute', frac: compute / total },
      { key: 'disk', frac: disk / total },
      { key: 'mgmt', frac: mgmt / total },
      { key: 'other', frac: other / total },
    ];
  });
</script>

<div class="tiles">
  <section class="card tile" aria-labelledby="tile-speed">
    <h2 id="tile-speed" title={t('live.speed.hint')}>{t('metrics.speed')}</h2>
    <p class="value figure">{formatNum(m.tokS, 1)} <small>tok/s</small></p>
    <Sparkline values={m.tokHistory} label={t('metrics.speed.history')} />
    <p class="note muted">{done ? t('metrics.speed.avg', { rate: formatNum(done.tok_s, 1) }) : t('metrics.waiting')}</p>
  </section>
  <section class="card tile" aria-labelledby="tile-disk">
    <h2 id="tile-disk" title={t('live.disk.hint')}>{t('metrics.disk')}</h2>
    <p class="value figure">{formatNum(m.readMibS, 0)} <small>MiB/s</small></p>
    <Sparkline values={m.readHistory} label={t('metrics.disk.history')} />
    <p class="note muted">{m.flashWait === null ? t('metrics.waiting') : t('metrics.disk.wait', { pct: formatPct(m.flashWait) })}</p>
  </section>
  <section class="card tile" aria-labelledby="tile-mem">
    <h2 id="tile-mem">{t('metrics.memory')}</h2>
    <!-- 0 is "the platform cannot report it" (the engine's convention), not an empty process. -->
    <p class="value figure">{last && last.rss_mib > 0 ? formatMib(last.rss_mib) : '–'}</p>
    {#if memFrac !== null}<meter min="0" max="1" value={memFrac} aria-label={t('metrics.memory')}></meter>{/if}
    <p class="note muted">{last ? t('metrics.memory.free', { free: formatMib(last.mem_available_mib) }) : t('metrics.waiting')}</p>
  </section>
  <section class="card tile" aria-labelledby="tile-cache">
    <h2 id="tile-cache" title={t('live.cache.hint')}>{t('metrics.cache')}</h2>
    <p class="value figure">{!last ? '–' : last.cache_hit_pct < 0 ? t('metrics.cache.none') : `${formatNum(last.cache_hit_pct, 0)}%`}</p>
    {#if last && last.cache_hit_pct >= 0}<meter min="0" max="100" value={last.cache_hit_pct} aria-label={t('metrics.cache')}></meter>{/if}
    <p class="note muted">{last && last.cache_budget_mib > 0 ? t('metrics.cache.budget', { size: formatMib(last.cache_budget_mib) }) : t('metrics.waiting')}</p>
  </section>
</div>

{#if split}
  <section class="card block" aria-labelledby="split-title">
    <h2 id="split-title">{t('metrics.split')}</h2>
    <div class="bar" role="img" aria-label={split.map((s) => `${t(`metrics.split.${s.key}`)} ${Math.round(s.frac * 100)}%`).join(', ')}>
      {#each split as s (s.key)}<span class="seg {s.key}" style:width="{(s.frac * 100).toFixed(2)}%"></span>{/each}
    </div>
    <ul class="legend">
      {#each split as s (s.key)}
        <li title={t(`metrics.split.${s.key}.hint`)}><span class="swatch {s.key}"></span>{t(`metrics.split.${s.key}`)} <span class="figure muted">{Math.round(s.frac * 100)}%</span></li>
      {/each}
    </ul>
  </section>
{/if}

{#if done}
  <section class="card block" aria-labelledby="last-title">
    <h2 id="last-title">{t('metrics.last')}</h2>
    <DoneSummary {done} />
  </section>
{/if}

<style>
  .tiles {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(100%, 13rem), 1fr));
    gap: var(--s3);
    margin-bottom: var(--s3);
  }
  .tile {
    display: flex;
    flex-direction: column;
    gap: var(--s2);
  }
  .tile h2,
  .block h2 {
    margin: 0;
    font-size: 0.8125rem;
    font-weight: 600;
    color: var(--muted);
  }
  .value {
    margin: 0;
    font-size: 1.75rem;
    font-weight: 600;
    letter-spacing: -0.02em;
  }
  .value small {
    font-size: 0.875rem;
    color: var(--muted);
    font-weight: 500;
  }
  .note {
    margin: 0;
    font-size: 0.75rem;
  }
  meter {
    width: 100%;
  }
  .block {
    margin-bottom: var(--s3);
  }
  .block h2 {
    margin-bottom: var(--s3);
  }
  .bar {
    display: flex;
    height: 0.75rem;
    border-radius: var(--pill);
    overflow: hidden;
    background: var(--surface-2);
    gap: 2px;
  }
  .seg {
    display: block;
    height: 100%;
  }
  .compute {
    background: var(--accent);
  }
  .disk {
    background: var(--warn);
  }
  .mgmt {
    background: var(--ok);
  }
  .other {
    background: var(--line-strong);
  }
  .legend {
    list-style: none;
    display: flex;
    gap: var(--s4);
    flex-wrap: wrap;
    padding: 0;
    margin: var(--s3) 0 0;
    font-size: 0.8125rem;
  }
  .legend li {
    display: flex;
    align-items: center;
    gap: var(--s1);
    cursor: help;
  }
  .swatch {
    width: 0.625rem;
    height: 0.625rem;
    border-radius: 2px;
  }
</style>
