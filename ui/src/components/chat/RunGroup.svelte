<script lang="ts">
  // A run of tool calls as one line ("Read 3 files, edited 2 files · +14 −3"), open while it runs or
  // waits for an answer and folded once it is done; one call alone is shown as itself.
  import Icon from '../Icon.svelte';
  import HelperRow from './HelperRow.svelte';
  import ToolCard from './ToolCard.svelte';
  import type { Item } from '../../lib/agent';
  import { runSummary, summaryText } from '../../lib/items';
  import { t } from '../../stores/prefs.svelte';

  let { items, conversation }: { items: Item[]; conversation: string } = $props();
  let summary = $derived(runSummary(items));
  let line = $derived(summaryText(summary, t));
  let live = $derived(summary.pending > 0);
  let userOpen = $state<boolean | null>(null);
  let open = $derived(userOpen ?? live);
</script>

{#snippet one(it: Item)}
  {#if it.kind === 'tool'}
    <ToolCard item={it} {conversation} />
  {:else if it.kind === 'helper'}
    <HelperRow item={it} />
  {/if}
{/snippet}

{#if items.length === 1}
  {@render one(items[0])}
{:else}
  <section class="run">
    <button type="button" class="ghost summary" aria-expanded={open} onclick={() => (userOpen = !open)}>
      <Icon name={open ? 'chevronDown' : 'chevronRight'} size={16} />
      <span class="line">{line || t('run.steps', { n: items.length })}</span>
      {#if live}<span class="badge info">{t('tool.state.running')}</span>{/if}
      {#if summary.failed}<span class="badge err">{t('run.failed', { n: summary.failed })}</span>{/if}
    </button>
    {#if open}
      <div class="calls">
        {#each items as it (it.id)}{@render one(it)}{/each}
      </div>
    {:else}
      <!-- An approval never hides inside a folded run. -->
      {#each items as it (it.id)}
        {#if it.kind === 'tool' && it.state === 'waiting'}<ToolCard item={it} {conversation} />{/if}
      {/each}
    {/if}
  </section>
{/if}

<style>
  .run {
    display: flex;
    flex-direction: column;
    gap: var(--s2);
  }
  .summary {
    align-self: flex-start;
    justify-content: flex-start;
    color: var(--muted);
    font-weight: 500;
    font-size: 0.875rem;
    padding-left: var(--s1);
    white-space: normal;
    text-align: left;
  }
  .summary:hover:not(:disabled) {
    color: var(--text);
  }
  .calls {
    display: flex;
    flex-direction: column;
    gap: var(--s2);
    padding-left: var(--s4);
    border-left: 2px solid var(--line);
    margin-left: var(--s3);
  }
</style>
