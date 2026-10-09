<script lang="ts">
  import { sparklinePoints } from '../lib/metrics';

  let { values, label, width = 160, height = 32 }: { values: number[]; label: string; width?: number; height?: number } =
    $props();
  let points = $derived(sparklinePoints(values, width, height));
</script>

<svg viewBox="0 0 {width} {height}" preserveAspectRatio="none" role="img" aria-label={label}>
  <title>{label}</title>
  {#if points}
    <polyline {points} fill="none" stroke="currentColor" stroke-width="1.5" vector-effect="non-scaling-stroke" />
  {/if}
</svg>

<style>
  svg {
    width: 100%;
    height: 32px;
    color: var(--accent);
    background: var(--surface-2);
    border-radius: 4px;
    display: block;
  }
</style>
