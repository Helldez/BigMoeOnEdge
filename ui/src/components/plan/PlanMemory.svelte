<script lang="ts">
  // One bar per memory pool: what the run holds, against what it may hold. The rows and their words
  // are the plan's ledger; the colour follows a row's position, never its name, from the series
  // palette so neighbouring parts stay apart.
  import { formatBytes } from '../../lib/format';
  import { memoryCells, memorySegments } from '../../lib/plan';
  import type { PlanMemoryPool } from '../../lib/types';
  import { t } from '../../lib/text';

  let { pools }: { pools: PlanMemoryPool[] } = $props();

  const SERIES = 6;
  // The colour as a class (s1..s6, free): no style attribute, as the cell meter.
  const tone = (slot: number) => (slot < 0 ? 'free' : `s${(slot % SERIES) + 1}`);
</script>

{#each pools as pool, pi (pi)}
  {@const segs = memorySegments(pool)}
  {@const turn = pool.rows.filter((r) => r.in_turn)}
  <div class="pool">
    <div class="head">
      <span>{pool.label ?? pool.name}{#if pool.estimated}<span class="muted"> {t('plan.memory.estimated')}</span>{/if}</span>
      {#if pool.cap_bytes > 0}<span class="muted"><span class="figure cap">{formatBytes(pool.cap_bytes)}</span> {t('plan.memory.mayHold')}</span>{/if}
    </div>
    <div class="cells" role="img" aria-label={segs.map((s) => `${s.label} ${formatBytes(s.bytes)}`).join(', ')}>
      {#each memoryCells(segs) as slot, i (i)}<i class={tone(slot)}></i>{/each}
    </div>
    <ul class="legend">
      {#each segs as s, i (i)}
        <li>
          <span class="swatch {tone(s.slot)}"></span>
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
  /* The pool counted in cells, as every share the engine measures (docs/design.md). */
  .cells {
    display: grid;
    grid-template-columns: repeat(40, 1fr);
    gap: 2px;
    height: 20px;
  }
  .cells i {
    border-radius: 1px;
  }
  .s1 { background: var(--series-1); }
  .s2 { background: var(--series-2); }
  .s3 { background: var(--series-3); }
  .s4 { background: var(--series-4); }
  .s5 { background: var(--series-5); }
  .s6 { background: var(--series-6); }
  .free { background: var(--surface-2); }
  .legend {
    list-style: none;
    margin: 0;
    padding: 0;
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(170px, 1fr));
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
    border-radius: 1px;
  }
  .swatch.free {
    box-shadow: inset 0 0 0 1px var(--line-strong);
  }
  .note {
    margin: 0;
    font-size: 0.9rem;
  }
</style>
