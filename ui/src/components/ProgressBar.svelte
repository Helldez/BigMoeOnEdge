<script lang="ts">
  import { formatBytes, progress } from '../lib/format';

  let { received, total, label }: { received: number; total: number; label: string } = $props();
  let frac = $derived(progress(received, total));
</script>

<div class="wrap">
  <progress max="1" value={frac ?? undefined} aria-label={label}></progress>
  <span class="muted">
    {formatBytes(received)}{total > 0 ? ` of ${formatBytes(total)} (${((frac ?? 0) * 100).toFixed(1)}%)` : ''}
  </span>
</div>

<style>
  .wrap {
    display: flex;
    flex-direction: column;
    gap: 0.2rem;
    font-size: 0.85rem;
  }
  progress {
    width: 100%;
    height: 0.6rem;
    accent-color: var(--accent);
  }
</style>
