<script lang="ts">
  // The rate and where the time goes, one line, while an answer streams and after; the full
  // card is on the Metrics page. The same line the phone shows above its prompt.
  import { formatNum } from '../../lib/format';
  import { server } from '../../stores/server.svelte';

  let m = $derived(server.metrics);
  let last = $derived(m.last);
  let done = $derived(server.lastDone);
  // The flash term is the one the server chose as a share of the wall (the stall under overlap,
  // the blocking read in serial): the summed lane time can exceed the wall and is not a share of it.

  let text = $derived.by(() => {
    const parts: string[] = [];
    if (done) {
      parts.push(`${formatNum(done.tok_s, 1)} tok/s avg`);
      const wall = done.tok_s > 0 ? 1 / done.tok_s : 0;
      if (wall > 0) {
        parts.push(`compute ${Math.round((100 * done.compute_s_tok) / wall)}%`);
        parts.push(`flash ${Math.round((100 * (done.flash_wait_s_tok ?? 0)) / wall)}%`);
      }
      if (done.cache_hit_pct >= 0) parts.push(`cache ${Math.round(done.cache_hit_pct)}%`);
    } else if (last) {
      parts.push(`${formatNum(m.tokS, 1)} tok/s`);
      if (last.wall_ms > 0) {
        parts.push(`compute ${Math.round((100 * last.compute_ms) / last.wall_ms)}%`);
        parts.push(`flash ${Math.round((100 * (last.flash_wait_ms ?? 0)) / last.wall_ms)}%`);
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
