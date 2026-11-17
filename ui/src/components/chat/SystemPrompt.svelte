<script lang="ts">
  // What the engine tells the model before a conversation (#625), read from the server when the
  // dialog opens: the text the next message sends, so nothing the model reads is hidden. Read-only:
  // a client that wants its own sends it as a system message over the API.
  import Dialog from '../Dialog.svelte';
  import { agentApi, type SystemPrompt } from '../../lib/agentApi';
  import { t, tp } from '../../lib/text';

  let { open = $bindable(false), conversation }: { open?: boolean; conversation: string } = $props();

  let prompt = $state<SystemPrompt | null>(null);
  let error = $state('');

  async function load() {
    prompt = null;
    error = '';
    try {
      prompt = await agentApi.systemPrompt(conversation);
    } catch (err) {
      error = err instanceof Error ? err.message : String(err);
    }
  }
  $effect(() => {
    if (open && conversation) load();
  });
</script>

<Dialog bind:open wide title={t('system.title')}>
  <p class="muted small">{t('system.lead')}</p>
  {#if error}
    <p class="err small" role="alert">{t('system.loadFailed')} {error}</p>
  {:else if !prompt}
    <span class="skeleton"></span><span class="skeleton"></span><span class="skeleton short"></span>
  {:else if prompt.external}
    <p>{t('system.external')}</p>
  {:else if prompt.text === ''}
    <p>{t('system.none')}</p>
  {:else}
    <pre class="mono">{prompt.text}</pre>
    {#if prompt.tokens !== null}<p class="muted small figure">{tp('system.tokens', prompt.tokens)}</p>{/if}
  {/if}
  {#snippet actions()}
    <button type="button" class="primary" onclick={() => (open = false)}>{t('action.close')}</button>
  {/snippet}
</Dialog>

<style>
  pre {
    white-space: pre-wrap;
    overflow-wrap: anywhere;
    max-height: 60vh;
    overflow: auto;
    margin: 0;
    padding: var(--s3);
    background: var(--hover);
    border-radius: var(--radius);
  }
  .err {
    color: var(--err);
  }
</style>
