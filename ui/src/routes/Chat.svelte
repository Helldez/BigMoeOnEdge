<script lang="ts">
  import Composer from '../components/chat/Composer.svelte';
  import LiveLine from '../components/chat/LiveLine.svelte';
  import MessageView from '../components/chat/MessageView.svelte';
  import { chat } from '../stores/chat.svelte';

  let log: HTMLElement | undefined = $state();

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

  const stamp = (ms: number) =>
    new Date(ms).toLocaleString(undefined, { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' });
</script>

<div class="chat">
  <aside class="conversations card" aria-label="Conversations">
    <div class="side-head">
      <h2>Conversations</h2>
      <button type="button" onclick={() => chat.newChat()} disabled={chat.busy}>New</button>
    </div>
    {#if chat.conversations.length === 0}
      <p class="muted small">Nothing saved yet. Conversations are kept in this browser.</p>
    {/if}
    <ul>
      {#each chat.conversations as c (c.id)}
        <li class:open={c.id === chat.current.id}>
          <button type="button" class="pick" onclick={() => chat.open(c.id)} disabled={chat.busy}>
            <span class="title">{c.title}</span>
            <span class="meta muted">{stamp(c.updatedAt)} · {c.turnCount} messages{c.model ? ` · ${c.model}` : ''}</span>
          </button>
          <button type="button" class="link del" onclick={() => chat.delete(c.id)} disabled={chat.busy && c.id === chat.current.id} aria-label="Delete {c.title}">Delete</button>
        </li>
      {/each}
    </ul>
  </aside>
  <section class="conversation" aria-label="Conversation">
    <div class="toolbar">
      <h1>{chat.current.title || 'New chat'}</h1>
      <span class="spacer"></span>
      {#if chat.turns.length > 0}
        <button type="button" onclick={() => chat.newChat()} disabled={chat.busy}>New chat</button>
      {/if}
    </div>
    <div class="log" bind:this={log} aria-live="polite">
      {#if chat.turns.length === 0}
        <p class="muted empty">Ask anything. The model runs on this machine; load one from Models if none is loaded, then type below.</p>
      {/if}
      {#each chat.turns as turn, i (turn.id)}
        <MessageView {turn} streaming={chat.busy && i === chat.turns.length - 1} last={i === chat.turns.length - 1} />
      {/each}
    </div>
    {#if chat.contextFull}
      <p class="banner error" role="alert">
        This conversation fills the model's context. Start a new chat, or load the model with a larger Context size.
        <button type="button" onclick={() => chat.newChat()}>New chat</button>
      </p>
    {:else if chat.error}
      <p class="banner error" role="alert">
        {chat.error}
        {#if chat.noModel}
          <a href="#/models">Models</a>
        {:else if chat.retryable}
          <button type="button" onclick={() => chat.retry()} disabled={chat.busy}>Send again</button>
        {/if}
        <button type="button" onclick={() => chat.clearError()}>Dismiss</button>
      </p>
    {/if}
    <LiveLine />
    <Composer />
  </section>
</div>

<style>
  .chat {
    display: grid;
    grid-template-columns: 260px minmax(0, 1fr);
    gap: 1rem;
    height: calc(100vh - 8rem);
    min-height: 26rem;
  }
  .conversations {
    overflow-y: auto;
    min-height: 0;
    padding: 0.6rem 0.6rem;
  }
  .side-head {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    margin-bottom: 0.4rem;
  }
  .side-head h2 {
    margin: 0;
    font-size: 0.95rem;
    flex: 1;
  }
  .conversations ul {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 0.2rem;
  }
  .conversations li {
    display: flex;
    align-items: center;
    gap: 0.3rem;
    border-radius: var(--radius);
    padding: 0.2rem 0.3rem;
  }
  .conversations li.open {
    background: var(--surface-2);
  }
  .pick {
    flex: 1;
    min-width: 0;
    text-align: left;
    border: none;
    background: none;
    padding: 0.2rem 0.3rem;
    display: flex;
    flex-direction: column;
    gap: 0.1rem;
  }
  .pick .title {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .pick .meta {
    font-size: 0.75rem;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .del {
    font-size: 0.8rem;
  }
  .small {
    font-size: 0.85rem;
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
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    max-width: 70%;
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
  @media (max-width: 900px) {
    .chat {
      grid-template-columns: minmax(0, 1fr);
      height: auto;
    }
    .conversations {
      max-height: 12rem;
    }
    .log {
      max-height: 60vh;
    }
  }
</style>
