<script lang="ts">
  import { renderMarkdown } from '../../lib/markdown';
  import { formatNum } from '../../lib/format';
  import type { Turn } from '../../stores/chat.svelte';

  let { turn, streaming = false }: { turn: Turn; streaming?: boolean } = $props();
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
    {#if turn.content}
      <div class="md">{@html html}</div>
    {:else if streaming && !turn.reasoning}
      <p class="muted">Waiting for the first token...</p>
    {/if}
    {#if turn.error}
      <p class="notice error">{turn.error}</p>
    {/if}
    {#if turn.finishReason === 'length'}
      <p class="notice">Stopped at the max tokens limit.</p>
    {:else if turn.finishReason === 'cancelled'}
      <p class="notice">Stopped.</p>
    {/if}
    {#if turn.summary}
      <p class="meta muted">
        {turn.summary.tokens} tokens, {formatNum(turn.summary.tok_s, 2)} tok/s, TTFT {formatNum(turn.summary.ttft_s, 2)} s
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
    background: var(--info-bg);
    max-width: 85%;
  }
  .assistant {
    background: var(--surface);
    border: 1px solid var(--border);
  }
  .user-text {
    white-space: pre-wrap;
  }
  .thinking {
    margin-bottom: 0.5rem;
    border-left: 3px solid var(--border);
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
    font-size: 0.8rem;
    margin: 0.4rem 0 0;
  }
</style>
