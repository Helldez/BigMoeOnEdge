<script lang="ts">
  // A measurement in progress: the stage running now and the ones already done, from the server's
  // `plan_progress` events, in the server's words. Before the first event arrives it says only that
  // it is measuring.
  import type { PlanProgress } from '../../lib/types';

  let { progress }: { progress: PlanProgress | null } = $props();

  const words = (p: PlanProgress) => p.label ?? p.step;
  let done = $state<string[]>([]);
  let last = '';
  $effect(() => {
    const p = progress;
    if (!p) return;
    if (p.index === 0) done = [];
    if (last && last !== words(p) && !done.includes(last)) done = [...done, last];
    last = words(p);
  });
  const left = $derived(progress?.count && progress.index !== undefined ? progress.count - progress.index - 1 : 0);
</script>

<div class="card progress" role="status">
  <p class="title">Measuring this machine</p>
  <progress max={progress?.count ?? 1} value={progress?.index ?? 0}></progress>
  <ul>
    {#each done as s (s)}
      <li><span class="badge ok">done</span> {s}</li>
    {/each}
    {#if progress}
      <li><span class="badge info">running</span> {words(progress)}</li>
      {#if left > 0}<li class="muted"><span class="badge">next</span> {left} more</li>{/if}
    {/if}
  </ul>
</div>

<style>
  .progress {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    margin-bottom: 1rem;
  }
  .title {
    margin: 0;
    font-size: 1.15rem;
  }
  progress {
    -webkit-appearance: none;
    appearance: none;
    width: 100%;
    height: 14px;
    border: var(--edge) solid var(--line);
    background: var(--surface-2);
  }
  progress::-webkit-progress-bar {
    background: var(--surface-2);
  }
  progress::-webkit-progress-value {
    background: var(--accent);
  }
  progress::-moz-progress-bar {
    background: var(--accent);
  }
  ul {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
  }
  li {
    display: flex;
    align-items: center;
    gap: 0.6rem;
  }
  .badge {
    min-width: 5.5rem;
    text-align: center;
  }
</style>
