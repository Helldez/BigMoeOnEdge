<script lang="ts" module>
  import type { IconName } from '../Icon.svelte';
  export const TOOL_ICON: Record<string, IconName> = {
    read_file: 'file',
    list_files: 'folder',
    search_files: 'search',
    write_file: 'file',
    edit_file: 'edit',
    run_command: 'terminal',
    fetch_url: 'globe',
    web_search: 'globe',
    use_skill: 'spark',
    delegate: 'helper',
    // Another agent's tool calls, by the kind the Agent Client Protocol gives them.
    read: 'file',
    edit: 'edit',
    delete: 'edit',
    move: 'edit',
    search: 'search',
    execute: 'terminal',
    fetch: 'globe',
    think: 'spark',
  };
  export const STATE_TONE: Record<string, string> = { running: 'info', waiting: 'warn', done: 'ok', denied: '', error: 'err' };
</script>

<script lang="ts">
  // One tool call (docs/agent.md, "Items"): its plain title and state, and on demand what it was
  // given and what it answered. Waiting, it is the approval card: nothing has happened yet, and
  // Approve or Deny is the person's answer.
  import Icon from '../Icon.svelte';
  import DiffView from './DiffView.svelte';
  import type { ToolItem } from '../../lib/agent';
  import { diffStat } from '../../lib/items';
  import { agent } from '../../stores/agent.svelte';
  import { preview } from '../../stores/preview.svelte';
  import { t } from '../../lib/text';

  let { item, conversation }: { item: ToolItem; conversation: string } = $props();
  let waiting = $derived(item.state === 'waiting');
  let open = $state(false);
  let stat = $derived(diffStat(item.diff));
  let arg = (k: string) => {
    const v = item.args?.[k];
    return typeof v === 'string' ? v : '';
  };
  // What the call would do, in the form a person checks it in: the command, the address, the text.
  let detail = $derived(arg('command') || arg('url') || arg('query') || '');
  let content = $derived(item.tool === 'write_file' ? arg('content') : '');
  let answering = $derived(!!agent.answering[item.id]);
</script>

<div class="tool" class:waiting role="group" aria-label={item.title}>
  <div class="head">
    <span class="ico"><Icon name={TOOL_ICON[item.tool] ?? 'more'} size={16} /></span>
    <span class="title">{item.title || item.tool}</span>
    {#if item.diff && (stat.added || stat.removed)}
      <span class="stat figure"><span class="add">+{stat.added}</span> <span class="del">−{stat.removed}</span></span>
    {/if}
    {#if item.state === 'running'}
      <span class="badge info"><span class="pulse"></span>{t('tool.state.running')}</span>
    {:else if item.state !== 'done'}
      <span class="badge {STATE_TONE[item.state] ?? ''}">{t(`tool.state.${item.state}`)}</span>
    {/if}
    {#if item.file && item.state === 'done'}
      <button type="button" class="ghost small" onclick={() => preview.show(conversation, item.file ?? '')}>
        <Icon name="eye" size={15} />{t('preview.open')}
      </button>
    {/if}
    {#if !waiting && (item.result || item.diff || detail || item.args)}
      <button type="button" class="ghost icon small" aria-expanded={open} aria-label={open ? t('action.hideDetails') : t('action.showDetails')} title={open ? t('action.hideDetails') : t('action.showDetails')} onclick={() => (open = !open)}>
        <Icon name={open ? 'chevronDown' : 'chevronRight'} size={16} />
      </button>
    {/if}
  </div>

  {#if waiting}
    <div class="approval">
      <p class="lead"><Icon name="shield" size={16} /><span>{t('approval.lead')}</span></p>
      {#if detail}<pre class="cmd">{detail}</pre>{/if}
      {#if item.diff}<DiffView diff={item.diff} />{:else if content}<pre class="cmd content">{content}</pre>{/if}
      <div class="buttons">
        <button type="button" class="primary" disabled={answering} onclick={() => agent.answer(item.id, 'approve')}>{t('approval.approve')}</button>
        <button type="button" disabled={answering} onclick={() => agent.answer(item.id, 'deny')}>{t('approval.deny')}</button>
        <span class="muted small">{t('approval.hint')}</span>
      </div>
    </div>
  {:else if open}
    <div class="body">
      {#if detail}<pre class="cmd">{detail}</pre>{:else if item.args && Object.keys(item.args).length}<pre class="cmd">{JSON.stringify(item.args, null, 2)}</pre>{/if}
      {#if item.diff}<DiffView diff={item.diff} />{/if}
      {#if item.result}<pre class="result">{item.result}</pre>{/if}
    </div>
  {/if}
</div>

<style>
  .tool {
    border: var(--edge) solid var(--line);
    border-radius: var(--radius);
    background: var(--surface);
  }
  .tool.waiting {
    border-color: var(--warn);
    box-shadow: var(--shadow-sm);
  }
  .head {
    display: flex;
    align-items: center;
    gap: var(--s2);
    min-height: 2.5rem;
    padding: var(--s1) var(--s2) var(--s1) var(--s3);
    font-size: 0.875rem;
  }
  .ico {
    color: var(--muted);
    display: inline-flex;
  }
  .title {
    flex: 1;
    min-width: 0;
    overflow-wrap: anywhere;
  }
  .stat {
    font-family: var(--mono);
    font-size: 0.75rem;
  }
  .add {
    color: var(--ok);
  }
  .del {
    color: var(--err);
  }
  .pulse {
    width: 0.5rem;
    height: 0.5rem;
    border-radius: 50%;
    background: var(--accent);
    animation: pulse 1.2s ease-in-out infinite;
  }
  @keyframes pulse {
    50% {
      opacity: 0.3;
    }
  }
  .body,
  .approval {
    border-top: var(--edge) solid var(--line);
    padding: var(--s3);
    display: flex;
    flex-direction: column;
    gap: var(--s2);
  }
  .approval {
    background: var(--warn-tint);
    border-radius: 0 0 var(--radius) var(--radius);
  }
  .lead {
    display: flex;
    gap: var(--s2);
    align-items: flex-start;
    margin: 0;
    color: var(--text);
    font-size: 0.875rem;
  }
  .cmd,
  .result {
    margin: 0;
    font-size: 0.8125rem;
    max-height: 18rem;
    overflow: auto;
    white-space: pre-wrap;
    overflow-wrap: anywhere;
  }
  .content {
    max-height: 12rem;
  }
  .buttons {
    display: flex;
    align-items: center;
    gap: var(--s2);
    flex-wrap: wrap;
  }
  .approval :global(.diff) {
    background: var(--surface);
  }
</style>
