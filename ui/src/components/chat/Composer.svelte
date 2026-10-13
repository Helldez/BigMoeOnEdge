<script lang="ts">
  import { chat } from '../../stores/chat.svelte';
  import { server } from '../../stores/server.svelte';

  // The only parameters the composer names: they are sent per request as max_tokens and think.
  const N_PREDICT = 'n-predict';
  const THINK = 'think';

  let text = $state('');
  let maxTokens = $state<number | null>(null);
  let think = $state(false);
  let seeded = false;

  let nPredictSpec = $derived(server.params?.find((p) => p.key === N_PREDICT));
  let hasThink = $derived(server.config ? THINK in server.config.values : false);

  // Initial values come from the config, once; later config changes do not overwrite edits.
  $effect(() => {
    const values = server.config?.values;
    if (seeded || !values) return;
    seeded = true;
    const n = values[N_PREDICT];
    if (typeof n === 'number') maxTokens = n;
    think = values[THINK] === true;
  });

  // A message the server refused goes back into the box.
  $effect(() => {
    if (chat.restore) {
      if (!text) text = chat.restore;
      chat.restore = '';
    }
  });

  let canSend = $derived(server.ready && !chat.busy && text.trim() !== '');

  function send() {
    if (!canSend) return;
    const msg = text;
    text = '';
    const n = maxTokens !== null && Number.isFinite(maxTokens) ? maxTokens : undefined;
    chat.send(msg, n, hasThink ? think : undefined);
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
  <label class="visually-hidden" for="composer-text">Message</label>
  <textarea
    id="composer-text"
    rows="3"
    placeholder={server.ready ? 'Message (Enter to send, Shift+Enter for a new line)' : 'Load a model to start chatting'}
    bind:value={text}
    onkeydown={onKey}
  ></textarea>
  <div class="row">
    <label class="inline">
      Max tokens
      <input
        type="number"
        min={nPredictSpec?.min}
        max={nPredictSpec?.max}
        step="1"
        bind:value={maxTokens}
      />
    </label>
    {#if hasThink}
      <label class="inline">
        <input type="checkbox" bind:checked={think} />
        Thinking
      </label>
    {/if}
    <span class="spacer"></span>
    {#if chat.busy}
      <button type="button" class="danger" onclick={() => chat.stop()}>Stop</button>
    {:else}
      <button type="submit" class="primary" disabled={!canSend}>Send</button>
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
  .inline input[type='number'] {
    width: 6.5rem;
  }
  .spacer {
    flex: 1;
  }
  .hint {
    margin: 0;
    font-size: 0.85rem;
  }
</style>
