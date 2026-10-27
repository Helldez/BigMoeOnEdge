<script lang="ts">
  // A helper the agent handed part of the job to: its task, its state and, once done, its summary;
  // its own conversation opens like any other.
  import Icon from '../Icon.svelte';
  import { STATE_TONE } from './ToolCard.svelte';
  import type { HelperItem } from '../../lib/agent';
  import { t } from '../../stores/prefs.svelte';

  let { item }: { item: HelperItem } = $props();
</script>

<div class="helper">
  <span class="ico"><Icon name="helper" size={16} /></span>
  <div class="text">
    <span class="title">{item.title}</span>
    {#if item.summary}<span class="muted small summary">{item.summary}</span>{/if}
  </div>
  <span class="badge {STATE_TONE[item.state] ?? ''}">{t(`tool.state.${item.state}`)}</span>
  {#if item.conversation}
    <a class="button small" href="#/chat/{encodeURIComponent(item.conversation)}">{t('helper.open')}</a>
  {/if}
</div>

<style>
  .helper {
    display: flex;
    align-items: center;
    gap: var(--s2);
    padding: var(--s2) var(--s3);
    border: var(--edge) dashed var(--line-strong);
    border-radius: var(--radius);
    font-size: 0.875rem;
  }
  .ico {
    color: var(--muted);
    display: inline-flex;
  }
  .text {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
  }
  .summary {
    overflow-wrap: anywhere;
  }
</style>
