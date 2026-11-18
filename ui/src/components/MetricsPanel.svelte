<script lang="ts">
  // The live figures: speed, disk reads, memory and the expert cache, each with its recent history,
  // then where a token's time goes and the last answer's account. Every figure comes from the
  // `token` and `done` events; a dash is "not measured yet", never a zero.
  import DoneSummary from './DoneSummary.svelte';
  import Sparkline from './Sparkline.svelte';
  import { formatMib, formatNum, formatPct } from '../lib/format';
  import { speedCheck } from '../lib/plan';
  import { timeShares } from '../lib/turnMetrics';
  import { server } from '../stores/server.svelte';
  import { t } from '../lib/text';

  let m = $derived(server.metrics);
  let last = $derived(m.last);
  let done = $derived(server.lastDone);
  // What the plan expects, said while nothing is measured yet, so an idle page reads as idle.
  let expected = $derived(server.plan?.available && !server.plan.error ? speedCheck(server.plan.predicted, server.plan.measured).predictedTokS : null);
  let memFrac = $derived(last && last.rss_mib > 0 && last.mem_available_mib > 0 ? last.rss_mib / (last.rss_mib + last.mem_available_mib) : null);

  // A token's wall time in the four terms the engine measures or leaves unclaimed (telemetry.md),
  // as whole percentages that add up to 100: the same split the figures under an answer show.
  let split = $derived(done ? timeShares(done) : []);
</script>

{#if !last && !done}
  <section class="card empty" aria-labelledby="metrics-empty">
    <h2 id="metrics-empty">{t('metrics.empty')}</h2>
    <p class="muted">{expected !== null ? t('metrics.empty.expected', { rate: formatNum(expected, 1) }) : t('metrics.empty.help')}</p>
  </section>
{:else}
<div class="tiles">
  <section class="card tile" aria-labelledby="tile-speed">
    <h2 id="tile-speed">{t('metrics.speed')}</h2>
    <p class="value figure">{formatNum(m.tokS, 1)} <small>tok/s</small></p>
    <Sparkline values={m.tokHistory} label={t('metrics.speed.history')} />
    <p class="note muted">{done ? t('metrics.speed.avg', { rate: formatNum(done.tok_s, 1) }) : t('metrics.waiting')}</p>
  </section>
  <section class="card tile" aria-labelledby="tile-disk">
    <h2 id="tile-disk">{t('metrics.disk')}</h2>
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
    <h2 id="tile-cache">{t('metrics.cache')}</h2>
    <p class="value figure">{!last ? '–' : last.cache_hit_pct < 0 ? t('metrics.cache.none') : `${formatNum(last.cache_hit_pct, 0)}%`}</p>
    {#if last && last.cache_hit_pct >= 0}<meter min="0" max="100" value={last.cache_hit_pct} aria-label={t('metrics.cache')}></meter>{/if}
    <p class="note muted">{last && last.cache_budget_mib > 0 ? t('metrics.cache.budget', { size: formatMib(last.cache_budget_mib) }) : t('metrics.waiting')}</p>
  </section>
</div>

{/if}

{#if split.length}
  <section class="card block" aria-labelledby="split-title">
    <h2 id="split-title">{t('metrics.split')}</h2>
    <div class="bar" role="img" aria-label={split.map((s) => `${t(`metrics.split.${s.key}`)} ${s.pct}%`).join(', ')}>
      {#each split as s (s.key)}<span class="seg {s.key}" style:width="{s.pct}%"></span>{/each}
    </div>
    <ul class="legend">
      {#each split as s (s.key)}
        <li><span class="swatch {s.key}"></span>{t(`metrics.split.${s.key}`)} <span class="figure muted">{s.pct}%</span></li>
      {/each}
    </ul>
  </section>
{/if}

{#if done}
  <section class="card block" aria-labelledby="last-title">
    <h2 id="last-title">{t('metrics.last')}</h2>
    <DoneSummary {done} nCtx={server.info?.model?.n_ctx ?? 0} />
  </section>
{/if}

<details class="meaning">
  <summary>{t('metrics.meaning')}</summary>
  <dl>
    <dt>{t('figure.speed')}</dt><dd>{t('figure.speed.hint')}</dd>
    <dt>{t('figure.cache')}</dt><dd>{t('figure.cache.hint')}</dd>
    <dt>{t('figure.disk')}</dt><dd>{t('figure.disk.hint')}</dd>
    {#each ['compute', 'mgmt', 'other'] as k (k)}<dt>{t(`metrics.split.${k}`)}</dt><dd>{t(`metrics.split.${k}.hint`)}</dd>{/each}
  </dl>
</details>

<style>
  .empty h2 {
    margin: 0 0 var(--s1);
    font-size: 1rem;
  }
  .empty p {
    margin: 0;
  }
  .meaning {
    margin-top: var(--s3);
    font-size: 0.875rem;
  }
  .meaning dl {
    display: grid;
    grid-template-columns: max-content 1fr;
    gap: var(--s1) var(--s4);
    margin: var(--s2) 0 0;
  }
  .meaning dt {
    font-weight: 550;
  }
  .meaning dd {
    margin: 0;
    color: var(--muted);
  }
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
    font-size: 0.8125rem;
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
