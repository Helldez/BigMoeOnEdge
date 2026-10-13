<script lang="ts">
  import Composer from '../components/chat/Composer.svelte';
  import MessageView from '../components/chat/MessageView.svelte';
  import MetricsPanel from '../components/MetricsPanel.svelte';
  import { chat } from '../stores/chat.svelte';

  let log: HTMLElement | undefined = $state();
  let showMetrics = $state(true);

  // Keep the newest text in view while streaming, unless the user scrolled up to read.
  $effect.pre(() => {
    const last = chat.turns[chat.turns.length - 1];
    void last?.content;
    void last?.reasoning;
    void chat.turns.length;
    if (!log) return;
    const nearBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 80;
    if (nearBottom) queueMicrotask(() => log && (log.scrollTop = log.scrollHeight));
  });
</script>

<div class="chat" class:no-metrics={!showMetrics}>
  <section class="conversation" aria-label="Conversation">
    <div class="toolbar">
      <h1>Chat</h1>
      <span class="spacer"></span>
      <button type="button" onclick={() => (showMetrics = !showMetrics)} aria-expanded={showMetrics}>
        {showMetrics ? 'Hide metrics' : 'Show metrics'}
      </button>
      <button type="button" onclick={() => chat.newChat()}>New chat</button>
    </div>
    <div class="log" bind:this={log} aria-live="polite">
      {#if chat.turns.length === 0}
        <p class="muted empty">Start a conversation. The whole history is sent with every message; the server continues its cache when it can.</p>
      {/if}
      {#each chat.turns as turn, i (turn.id)}
        <MessageView {turn} streaming={chat.busy && i === chat.turns.length - 1} />
      {/each}
    </div>
    {#if chat.error}
      <p class="banner error" role="alert">{chat.error}</p>
    {/if}
    <Composer />
  </section>
  {#if showMetrics}
    <aside class="metrics">
      <MetricsPanel />
    </aside>
  {/if}
</div>

<style>
  .chat {
    display: grid;
    grid-template-columns: minmax(0, 1fr) 320px;
    gap: 1rem;
    height: calc(100vh - 8rem);
    min-height: 26rem;
  }
  .chat.no-metrics {
    grid-template-columns: minmax(0, 1fr);
  }
  .conversation {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    min-height: 0;
    min-width: 0;
  }
  .toolbar {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    flex-wrap: wrap;
  }
  .toolbar h1 {
    margin: 0;
  }
  .spacer {
    flex: 1;
  }
  .log {
    flex: 1;
    overflow-y: auto;
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    padding-right: 0.25rem;
    min-height: 8rem;
  }
  .empty {
    margin: auto;
    text-align: center;
    max-width: 30rem;
  }
  .metrics {
    overflow-y: auto;
    min-height: 0;
  }
  @media (max-width: 900px) {
    .chat {
      grid-template-columns: minmax(0, 1fr);
      height: auto;
    }
    .log {
      max-height: 60vh;
    }
  }
</style>
