<script lang="ts">
  import DoneSummary from './DoneSummary.svelte';
  import Sparkline from './Sparkline.svelte';
  import { formatMib, formatNum, formatPct } from '../lib/format';
  import { server } from '../stores/server.svelte';

  let m = $derived(server.metrics);
  let last = $derived(m.last);
  let done = $derived(server.lastDone);
</script>

<section class="panel card" aria-label="Live metrics">
  <h2>Live metrics</h2>
  {#if !last && !done}
    <p class="muted">Metrics appear while the model generates.</p>
  {/if}
  <dl class="grid">
    <div>
      <dt>Decode</dt>
      <dd>{formatNum(m.tokS, 2)} <small>tok/s</small></dd>
    </div>
    <div>
      <dt>Flash read</dt>
      <dd>{formatNum(m.readMibS, 0)} <small>MiB/s</small></dd>
    </div>
    <div>
      <dt>Stall share</dt>
      <dd>{m.stall === null ? '-' : formatPct(m.stall)}</dd>
    </div>
    <div>
      <dt>Cache hit</dt>
      <dd>{!last ? '-' : last.cache_hit_pct < 0 ? 'no cache' : `${formatNum(last.cache_hit_pct, 1)}%`}</dd>
    </div>
    <div>
      <dt>TTFT</dt>
      <dd>{done ? `${formatNum(done.ttft_s, 2)} s` : '-'}</dd>
    </div>
    <div>
      <dt>Prefill</dt>
      <dd>{done ? `${formatNum(done.prefill_s, 2)} s` : '-'}</dd>
    </div>
    <div>
      <dt>Cache budget</dt>
      <dd>{last ? formatMib(last.cache_budget_mib) : '-'}</dd>
    </div>
    <div>
      <dt>RSS</dt>
      <!-- 0 is "the platform cannot report it" (the engine's convention), not an empty process. -->
      <dd>{last && last.rss_mib > 0 ? formatMib(last.rss_mib) : '-'}</dd>
    </div>
    <div>
      <dt>Available memory</dt>
      <dd>{last ? formatMib(last.mem_available_mib) : '-'}</dd>
    </div>
    <div>
      <dt>Step</dt>
      <dd>{last ? `${last.step} / ${last.steps}` : '-'}</dd>
    </div>
  </dl>
  <div class="spark">
    <span class="muted">tok/s</span>
    <Sparkline values={m.tokHistory} label="Tokens per second, recent tokens" />
  </div>
  <div class="spark">
    <span class="muted">read MiB/s</span>
    <Sparkline values={m.readHistory} label="Flash read MiB per second, recent tokens" />
  </div>
  {#if done}
    <h3>Last generation</h3>
    <DoneSummary {done} />
  {/if}
</section>

<style>
  .panel h2 {
    margin-top: 0;
  }
  h3 {
    font-size: 0.95rem;
    margin: 1rem 0 0.4rem;
  }
  .grid {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: 0.5rem 0.75rem;
    margin: 0 0 0.75rem;
  }
  dt {
    font-size: 0.75rem;
    color: var(--muted);
  }
  dd {
    margin: 0;
    font-size: 1.05rem;
    font-weight: 600;
    font-variant-numeric: tabular-nums;
  }
  small {
    font-weight: 400;
    color: var(--muted);
  }
  .spark {
    margin-bottom: 0.5rem;
    font-size: 0.75rem;
  }
</style>
