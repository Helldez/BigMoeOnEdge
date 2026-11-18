<script lang="ts">
  // While an answer is being written: its writing speed, live, where the finished answer's line
  // (TurnFigures) will land, in the same words.
  import { formatNum } from '../../lib/format';
  import { server } from '../../stores/server.svelte';
  import { t } from '../../lib/text';

  let m = $derived(server.metrics);
</script>

{#if m.last}
  <p class="live figure" aria-live="off">{t('figure.tg', { rate: formatNum(m.tokS, 1) })}</p>
{/if}

<style>
  .live {
    margin: var(--s2) 0 0;
    padding: 0 var(--s2);
    font-size: 0.8125rem;
    font-weight: 550;
    color: var(--muted);
  }
</style>
