<script lang="ts">
  // The rate and where the time goes, one line, while an answer streams and after; the full
  // card is on the Metrics page. The same line the phone shows above its prompt.
  import { formatNum } from '../../lib/format';
  import { server } from '../../stores/server.svelte';

  let m = $derived(server.metrics);
  let last = $derived(m.last);
  let done = $derived(server.lastDone);
  // The flash term that is wall-additive: the stall under overlap, the blocking read in serial.
  // The summed lane time (io under overlap) can exceed the wall and is not a share of it.
  let overlap = $derived(server.config?.values['overlap'] === true);

  let text = $derived.by(() => {
    const parts: string[] = [];
    if (done) {
      parts.push(`${formatNum(done.tok_s, 1)} tok/s avg`);
      const wall = done.tok_s > 0 ? 1 / done.tok_s : 0;
      if (wall > 0) {
        parts.push(`compute ${Math.round((100 * done.compute_s_tok) / wall)}%`);
        parts.push(`flash ${Math.round((100 * (overlap ? done.stall_s_tok : done.io_s_tok)) / wall)}%`);
      }
      if (done.cache_hit_pct >= 0) parts.push(`cache ${Math.round(done.cache_hit_pct)}%`);
    } else if (last) {
      parts.push(`${formatNum(m.tokS, 1)} tok/s`);
      if (last.wall_ms > 0) {
        parts.push(`compute ${Math.round((100 * last.compute_ms) / last.wall_ms)}%`);
        parts.push(`flash ${Math.round((100 * (overlap ? last.stall_ms : last.io_ms)) / last.wall_ms)}%`);
      }
      if (last.cache_hit_pct >= 0) parts.push(`cache ${Math.round(last.cache_hit_pct)}%`);
      if (last.steps > 0) parts.push(`${last.step}/${last.steps}`);
    }
    return parts.join(' · ');
  });
</script>

{#if text}
  <p class="live muted" aria-live="polite">{text}</p>
{/if}

<style>
  .live {
    margin: 0;
    font-family: var(--pixel);
    font-size: 0.85rem;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
</style>
