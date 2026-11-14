<script lang="ts">
  // One bar per memory pool: what the run holds, against what it may hold. The rows and their words
  // are the plan's ledger; the colour follows a row's position, never its name, from the series
  // palette so neighbouring parts stay apart.
  import { formatBytes } from '../../lib/format';
  import { memorySegments } from '../../lib/plan';
  import type { PlanMemoryPool } from '../../lib/types';
  import { t } from '../../lib/text';

  let { pools }: { pools: PlanMemoryPool[] } = $props();

  const SERIES = 6;
  const colour = (slot: number) => (slot < 0 ? 'var(--surface-2)' : `var(--series-${(slot % SERIES) + 1})`);
</script>

{#each pools as pool, pi (pi)}
  {@const segs = memorySegments(pool)}
  {@const turn = pool.rows.filter((r) => r.in_turn)}
  <div class="pool">
    <div class="head">
      <span>{pool.label ?? pool.name}{#if pool.estimated}<span class="muted"> {t('plan.memory.estimated')}</span>{/if}</span>
      {#if pool.cap_bytes > 0}<span class="muted"><span class="figure cap">{formatBytes(pool.cap_bytes)}</span> {t('plan.memory.mayHold')}</span>{/if}
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
          {#if s.locked}<span class="badge">{t('plan.memory.locked')}</span>{/if}
        </li>
      {/each}
    </ul>
    {#each turn as row, i (i)}
      <p class="muted note">{t('plan.memory.inTurn', { name: row.label ?? row.name, size: formatBytes(row.bytes) })}</p>
    {/each}
    {#if pi === 0 && pool.cache_lock_unverified}
      <p class="muted note">{t('plan.memory.lockUnverified')}</p>
    {:else if pi === 0 && pool.locked_bytes === 0}
      <p class="muted note">{t('plan.memory.nothingLocked')}</p>
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
    border: var(--edge) solid var(--line);
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
    grid-template-columns: repeat(auto-fit, minmax(240px, 1fr));
    gap: 0.4rem 1rem;
  }
  .legend li {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }
  /* A size never breaks across lines; the row's name wraps instead. */
  .legend .figure {
    white-space: nowrap;
  }
  .swatch {
    flex: 0 0 12px;
    height: 12px;
    border: 1px solid var(--line);
  }
  .note {
    margin: 0;
    font-size: 0.9rem;
  }
</style>
