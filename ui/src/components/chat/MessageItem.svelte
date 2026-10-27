<script lang="ts">
  // A message or a notice of a conversation (docs/agent.md, "Items"). The person's words verbatim,
  // editable and sent again from there; the model's as sanitised Markdown with its reasoning folded
  // above and the turn's figures under; a notice as a line that says why something did not happen.
  import Icon from '../Icon.svelte';
  import Markdown from './Markdown.svelte';
  import type { AssistantItem, NoticeItem, UserItem } from '../../lib/agent';
  import { agent } from '../../stores/agent.svelte';
  import { t } from '../../stores/prefs.svelte';

  let { item, last = false }: { item: UserItem | AssistantItem | NoticeItem; last?: boolean } = $props();

  let editing = $state(false);
  let draft = $state('');
  let copied = $state(false);

  async function copy(text: string) {
    try {
      await navigator.clipboard.writeText(text);
      copied = true;
      setTimeout(() => (copied = false), 1500);
    } catch {
      copied = false;
    }
  }
  function startEdit(text: string) {
    draft = text;
    editing = true;
  }
  async function resend() {
    if (!draft.trim()) return;
    if (await agent.edit(item.id, draft)) editing = false;
  }
  function editKey(e: KeyboardEvent) {
    if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) {
      e.preventDefault();
      resend();
    } else if (e.key === 'Escape') editing = false;
  }
</script>

{#if item.kind === 'user'}
  <article class="user" aria-label={t('message.you')}>
    {#if editing}
      <div class="edit">
        <label class="visually-hidden" for="edit-{item.id}">{t('message.edit')}</label>
        <!-- svelte-ignore a11y_autofocus -->
        <textarea id="edit-{item.id}" rows="3" bind:value={draft} onkeydown={editKey} autofocus></textarea>
        <p class="muted small">{t('message.edit.hint')}</p>
        <div class="row">
          <span class="spacer"></span>
          <button type="button" onclick={() => (editing = false)}>{t('action.cancel')}</button>
          <button type="button" class="primary" disabled={!draft.trim() || agent.running} onclick={resend}>{t('message.edit.send')}</button>
        </div>
      </div>
    {:else}
      <div class="bubble">{item.text}</div>
      <div class="tools">
        <button type="button" class="ghost icon small" title={copied ? t('action.copied') : t('action.copy')} aria-label={t('action.copy')} onclick={() => copy(item.text)}>
          <Icon name={copied ? 'check' : 'copy'} size={15} />
        </button>
        <button type="button" class="ghost icon small" title={t('message.edit')} aria-label={t('message.edit')} disabled={agent.running} onclick={() => startEdit(item.text)}>
          <Icon name="edit" size={15} />
        </button>
      </div>
    {/if}
  </article>
{:else if item.kind === 'assistant'}
  <article class="assistant" aria-label={t('message.assistant')} aria-busy={item.streaming ? 'true' : undefined}>
    {#if item.reasoning}
      <details class="thinking" open={item.streaming && !item.text}>
        <summary><Icon name="brain" size={15} />{item.streaming && !item.text ? t('message.thinking.live') : t('message.thinking')}</summary>
        <div class="reasoning"><Markdown text={item.reasoning} /></div>
      </details>
    {/if}
    {#if item.text}
      <Markdown text={item.text} />
    {:else if item.streaming && !item.reasoning}
      <p class="muted waiting"><span class="skeleton bar"></span>{t('message.reading')}</p>
    {/if}
    {#if !item.streaming && item.text}
      <div class="foot">
        <button type="button" class="ghost small" onclick={() => copy(item.text)}><Icon name={copied ? 'check' : 'copy'} size={15} />{copied ? t('action.copied') : t('action.copy')}</button>
        {#if last}
          <button type="button" class="ghost small" disabled={agent.running} onclick={() => agent.regenerate()}><Icon name="refresh" size={15} />{t('message.regenerate')}</button>
        {/if}
        {#if agent.metrics[item.id]}
          <span class="metrics muted figure" title={t('message.metrics.hint')}>{agent.metrics[item.id]}</span>
        {/if}
      </div>
    {/if}
  </article>
{:else}
  <p class="notice {item.level}" role={item.level === 'error' ? 'alert' : undefined}>
    <Icon name={item.level === 'info' ? 'info' : 'alert'} size={16} /><span>{item.text}</span>
  </p>
{/if}

<style>
  .user {
    align-self: flex-end;
    max-width: min(85%, 40rem);
    display: flex;
    flex-direction: column;
    align-items: flex-end;
    gap: 2px;
  }
  .bubble {
    background: var(--surface-2);
    border-radius: var(--radius) var(--radius) 2px var(--radius);
    padding: var(--s2) var(--s3);
    white-space: pre-wrap;
    overflow-wrap: anywhere;
  }
  .tools {
    display: flex;
    gap: 2px;
    opacity: 0;
    transition: opacity var(--fast) var(--ease);
  }
  .user:hover .tools,
  .user:focus-within .tools {
    opacity: 1;
  }
  @media (pointer: coarse) {
    .tools {
      opacity: 1;
    }
  }
  .edit {
    width: min(40rem, 85vw);
    display: flex;
    flex-direction: column;
    gap: var(--s1);
  }
  .edit textarea {
    width: 100%;
    resize: vertical;
  }
  .edit p {
    margin: 0;
  }
  .assistant {
    max-width: 100%;
    min-width: 0;
  }
  .thinking {
    margin-bottom: var(--s3);
    border-left: 2px solid var(--line);
    padding-left: var(--s3);
  }
  .thinking summary {
    display: inline-flex;
    align-items: center;
    gap: var(--s1);
    font-size: 0.8125rem;
    color: var(--muted);
    font-weight: 550;
    list-style: none;
  }
  .thinking summary::-webkit-details-marker {
    display: none;
  }
  .reasoning {
    margin-top: var(--s2);
    color: var(--muted);
    font-size: 0.9rem;
  }
  .waiting {
    display: flex;
    align-items: center;
    gap: var(--s2);
    margin: 0;
  }
  .bar {
    width: 3rem;
    height: 0.5rem;
    min-height: 0.5rem;
  }
  .foot {
    display: flex;
    align-items: center;
    gap: var(--s1);
    flex-wrap: wrap;
    margin-top: var(--s2);
  }
  .foot button {
    color: var(--muted);
    font-weight: 500;
  }
  .metrics {
    font-size: 0.75rem;
    margin-left: var(--s2);
  }
  .notice {
    display: flex;
    align-items: flex-start;
    gap: var(--s2);
    margin: 0;
    padding: var(--s2) var(--s3);
    border-radius: var(--radius);
    background: var(--surface-2);
    font-size: 0.875rem;
  }
  .notice.warn {
    background: var(--warn-tint);
    color: var(--warn);
  }
  .notice.error {
    background: var(--err-tint);
    color: var(--err);
  }
</style>
