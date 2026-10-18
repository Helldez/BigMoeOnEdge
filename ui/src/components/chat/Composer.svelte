<script lang="ts">
  import { chat } from '../../stores/chat.svelte';
  import { server } from '../../stores/server.svelte';

  // The composer names no parameter: how many tokens to generate and whether to think are
  // request-scoped rows in Settings, and the server applies them to every message.
  let text = $state('');
  let options = $state('');

  let canSend = $derived(server.ready && !chat.busy && text.trim() !== '' && (!chat.choose || options.trim() !== ''));

  function send() {
    if (!canSend) return;
    const msg = text;
    text = '';
    if (chat.choose) {
      chat.decide(msg, options);
      return;
    }
    chat.send(msg);
  }

  function onKey(e: KeyboardEvent) {
    if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) {
      e.preventDefault();
      send();
    }
  }
</script>

<form
  class="composer"
  onsubmit={(e) => {
    e.preventDefault();
    send();
  }}
>
  <label class="visually-hidden" for="composer-text">{chat.choose ? 'Question' : 'Message'}</label>
  <textarea
    id="composer-text"
    rows="3"
    placeholder={!server.ready
      ? 'Load a model to start chatting'
      : chat.choose
        ? 'Question (the model picks one of the options below)'
        : 'Message (Enter to send, Shift+Enter for a new line)'}
    bind:value={text}
    onkeydown={onKey}
  ></textarea>
  {#if chat.choose}
    <label class="visually-hidden" for="composer-options">Options, one per line</label>
    <textarea id="composer-options" rows="3" placeholder="Options, one per line" bind:value={options}></textarea>
  {/if}
  <div class="row">
    <label class="inline" title="The model picks one of your options from a single prefill, with no decode: each option comes back with the probability the model put on it.">
      <input type="checkbox" bind:checked={chat.choose} disabled={chat.busy} />
      Choose from options
    </label>
    <span class="spacer"></span>
    {#if chat.busy}
      <button type="button" class="danger" onclick={() => chat.stop()}>Stop</button>
    {:else}
      <button type="submit" class="primary" disabled={!canSend}>{chat.choose ? 'Choose' : 'Send'}</button>
    {/if}
  </div>
  {#if !server.ready}
    <p class="muted hint">No model is ready. <a href="#/models">Load one in Models</a>.</p>
  {/if}
</form>

<style>
  .composer {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }
  textarea {
    width: 100%;
    resize: vertical;
    min-height: 4.5rem;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 0.9rem;
    flex-wrap: wrap;
  }
  .inline {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    font-size: 0.9rem;
  }
  .spacer {
    flex: 1;
  }
  .hint {
    margin: 0;
    font-size: 0.85rem;
  }
</style>
