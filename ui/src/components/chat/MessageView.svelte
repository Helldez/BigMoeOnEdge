<script lang="ts">
  import { renderMarkdown } from '../../lib/markdown';
  import { chat, type Turn } from '../../stores/chat.svelte';

  let { turn, streaming = false, last = false }: { turn: Turn; streaming?: boolean; last?: boolean } = $props();
  let copied = $state(false);
  async function copy() {
    try {
      await navigator.clipboard.writeText(turn.content);
      copied = true;
      setTimeout(() => (copied = false), 1500);
    } catch {
      copied = false;
    }
  }
  let html = $derived(turn.role === 'assistant' ? renderMarkdown(turn.content) : '');
  let reasoningHtml = $derived(turn.reasoning ? renderMarkdown(turn.reasoning) : '');
</script>

<article class="msg {turn.role}" aria-label={turn.role === 'user' ? 'You' : 'Assistant'}>
  {#if turn.role === 'user'}
    <div class="text user-text">{turn.content}</div>
  {:else}
    {#if turn.historyDropped}
      <p class="notice">Earlier turns could not be replayed and were dropped from the model's context.</p>
    {/if}
    {#if turn.reasoning}
      <details class="thinking" open={streaming && !turn.content}>
        <summary>Thinking</summary>
        <div class="md">{@html reasoningHtml}</div>
      </details>
    {/if}
    {#if turn.choices}
      <ul class="choices" aria-label="The model's choice">
        {#each turn.choices as c, i (c.label)}
          <li class:picked={i === turn.best}>
            <span class="label">{c.label})</span>
            <span class="text">{c.text}</span>
            <meter min="0" max="1" value={c.prob}></meter>
            <span class="prob">{Math.round(100 * c.prob)}%</span>
          </li>
        {/each}
      </ul>
      {#if turn.metrics}<p class="meta muted">{turn.metrics}</p>{/if}
    {:else if turn.content}
      <div class="md">{@html html}</div>
    {:else if streaming && !turn.reasoning}
      <p class="muted">{turn.choices === undefined && !turn.content ? 'Waiting for the first token...' : ''}</p>
    {/if}
    {#if turn.error}
      <p class="notice error">{turn.error}</p>
    {/if}
    {#if turn.finishReason === 'length'}
      <p class="notice">Stopped at the max tokens limit.</p>
    {:else if turn.finishReason === 'cancelled'}
      <p class="notice">Stopped.</p>
    {/if}
    {#if turn.metrics && !turn.choices}
      <p class="meta muted">{turn.metrics}</p>
    {/if}
    {#if !streaming && !turn.choices && turn.content}
      <p class="actions">
        <button type="button" class="link" onclick={copy}>{copied ? 'Copied' : 'Copy'}</button>
        {#if last}
          <button type="button" class="link" onclick={() => chat.regenerate()} disabled={chat.busy}>Regenerate</button>
        {/if}
      </p>
    {/if}
  {/if}
</article>

<style>
  .msg {
    max-width: 100%;
    padding: 0.6rem 0.9rem;
    border-radius: var(--radius);
    overflow-wrap: anywhere;
  }
  .user {
    align-self: flex-end;
    background: var(--surface-2);
    border: var(--edge) solid var(--border);
    max-width: 85%;
  }
  .assistant {
    background: var(--surface);
    border: var(--edge) solid var(--border);
    box-shadow: 4px 4px 0 var(--shadow);
  }
  .user-text {
    white-space: pre-wrap;
  }
  .thinking {
    margin-bottom: 0.5rem;
    border-left: 4px solid var(--accent-2);
    padding-left: 0.6rem;
    color: var(--muted);
  }
  .thinking summary {
    cursor: pointer;
    font-size: 0.85rem;
  }
  .md :global(p:first-child) {
    margin-top: 0;
  }
  .md :global(p:last-child) {
    margin-bottom: 0;
  }
  .md :global(pre) {
    white-space: pre;
  }
  .md :global(table) {
    display: block;
    overflow-x: auto;
  }
  .notice {
    font-size: 0.85rem;
    color: var(--warn);
    margin: 0.4rem 0 0;
  }
  .notice.error {
    color: var(--err);
  }
  .meta {
    font-size: 0.85rem;
    margin: 0.4rem 0 0;
    font-family: var(--pixel);
  }
  .actions {
    margin: 0.3rem 0 0;
    display: flex;
    gap: 0.9rem;
    font-size: 0.8rem;
  }
  .choices {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
  }
  .choices li {
    display: grid;
    grid-template-columns: 2rem minmax(0, 1fr) 6rem 3rem;
    align-items: center;
    gap: 0.5rem;
  }
  .choices li.picked {
    font-weight: 600;
  }
  .choices .text {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .choices meter {
    width: 100%;
  }
  .choices .prob {
    text-align: right;
    font-variant-numeric: tabular-nums;
  }
</style>
