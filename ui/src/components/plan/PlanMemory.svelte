<script lang="ts">
  // One bar per memory pool: what the run holds, against what it may hold. The rows and their words
  // are the plan's ledger; the colour follows a row's position, never its name.
  import { formatBytes } from '../../lib/format';
  import { memorySegments } from '../../lib/plan';
  import type { PlanMemoryPool } from '../../lib/types';

  let { pools }: { pools: PlanMemoryPool[] } = $props();

  const COLOURS = ['var(--accent-2)', 'var(--accent)', 'var(--muted)', 'var(--ok)', 'var(--warn)'];
  const colour = (slot: number) => (slot < 0 ? 'var(--surface-2)' : COLOURS[slot % COLOURS.length]);
</script>

{#each pools as pool, pi (pi)}
  {@const segs = memorySegments(pool)}
  {@const turn = pool.rows.filter((r) => r.in_turn)}
  <div class="pool">
    <div class="head">
      <span>{pool.label ?? pool.name}{#if pool.estimated}<span class="muted"> (an even share of the devices' sum)</span>{/if}</span>
      {#if pool.cap_bytes > 0}<span class="muted"><span class="figure cap">{formatBytes(pool.cap_bytes)}</span> it may hold</span>{/if}
    </div>
    <div class="bar" role="img" aria-label={segs.map((s) => `${s.label} ${formatBytes(s.bytes)}`).join(', ')}>
      {#each segs as s, i (i)}
        <span style="width: {(s.frac * 100).toFixed(2)}%; background: {colour(s.slot)}"></span>
      {/each}
    </div>
    <ul class="legend">
      {#each segs as s, i (i)}
        <li>
          <span class="swatch" style="background: {colour(s.slot)}"></span>
          <span class="figure">{formatBytes(s.bytes)}</span>
          <span class="muted">{s.label}</span>
          {#if s.locked}<span class="badge">locked</span>{/if}
        </li>
      {/each}
    </ul>
    {#each turn as t, i (i)}
      <p class="muted note">{t.label ?? t.name}: {formatBytes(t.bytes)} borrowed from the expert cache while a prompt runs, given back after.</p>
    {/each}
    {#if pi === 0 && pool.cache_lock_unverified}
      <p class="muted note">The system's lock limit is unmeasured. The expert cache will try to lock its pages when the run starts.</p>
    {:else if pi === 0 && pool.locked_bytes === 0}
      <p class="muted note">Nothing is locked in memory for this run: the system may take any of it back under pressure.</p>
    {/if}
  </div>
{/each}

<style>
  .pool {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }
  .pool + .pool {
    margin-top: 1rem;
  }
  .head {
    display: flex;
    justify-content: space-between;
    gap: 0.5rem;
    flex-wrap: wrap;
  }
  .cap {
    color: var(--text);
  }
  .bar {
    display: flex;
    height: 28px;
    border: var(--edge) solid var(--border);
    background: var(--surface-2);
    overflow: hidden;
  }
  .bar span + span {
    border-left: var(--edge) solid var(--bg);
  }
  .legend {
    list-style: none;
    margin: 0;
    padding: 0;
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(190px, 1fr));
    gap: 0.4rem 1rem;
  }
  .legend li {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }
  .swatch {
    flex: 0 0 12px;
    height: 12px;
    border: 1px solid var(--border);
  }
  .note {
    margin: 0;
    font-size: 0.9rem;
  }
</style>
