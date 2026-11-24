<script lang="ts">
  // The conversation's system prompt (#625): the person's own, which they write here, and what the
  // model reads before the conversation, exactly as the next message sends it. With a folder the
  // agent's lines come first; without one the person's text is all the model gets, as in llama.cpp's
  // web UI. A new conversation keeps it in the draft and sends it with the first message.
  import Dialog from '../Dialog.svelte';
  import { agentApi, type SystemPrompt } from '../../lib/agentApi';
  import { agent } from '../../stores/agent.svelte';
  import { t, tp } from '../../lib/text';

  let { open = $bindable(false) }: { open?: boolean } = $props();

  let prompt = $state<SystemPrompt | null>(null);
  let own = $state('');
  let error = $state('');
  let saving = $state(false);

  // What the server would send, read again after a save: only an existing conversation has one.
  async function load() {
    prompt = null;
    error = '';
    own = agent.system;
    if (!agent.currentId) return;
    try {
      prompt = await agentApi.systemPrompt(agent.currentId);
      own = prompt.own;
    } catch (err) {
      error = err instanceof Error ? err.message : String(err);
    }
  }
  $effect(() => {
    if (open) load();
  });

  async function save() {
    saving = true;
    error = '';
    const ok = await agent.setSystem(own);
    saving = false;
    if (!ok) {
      error = agent.actionError;
      return;
    }
    if (agent.currentId) await load();
    else open = false;
  }

  // The agent's lines are what the server adds before the person's text: shown only where there are some.
  const added = $derived(prompt && prompt.text !== prompt.own ? prompt.text : '');
  const changed = $derived(own !== agent.system);
</script>

<Dialog bind:open wide title={t('system.title')}>
  {#if prompt?.external}
    <p>{t('system.external')}</p>
  {:else}
    <label for="system-own" class="muted small">{t('system.lead')}</label>
    <textarea id="system-own" rows="6" bind:value={own} placeholder={t('system.placeholder')} disabled={saving}></textarea>
    {#if error}<p class="err small" role="alert">{t('system.failed')} {error}</p>{/if}
    {#if added}
      <details>
        <summary class="small">{t('system.sent')}</summary>
        <pre class="mono">{added}</pre>
      </details>
    {/if}
    {#if prompt && prompt.tokens !== null}<p class="muted small figure">{tp('system.tokens', prompt.tokens)}</p>{/if}
  {/if}
  {#snippet actions()}
    {#if !prompt?.external}
      <button type="button" class="primary" disabled={!changed || saving} onclick={save}>{t('action.save')}</button>
    {/if}
    <button type="button" class="ghost" onclick={() => (open = false)}>{t('action.close')}</button>
  {/snippet}
</Dialog>

<style>
  textarea {
    width: 100%;
    box-sizing: border-box;
    resize: vertical;
    font: inherit;
  }
  pre {
    white-space: pre-wrap;
    overflow-wrap: anywhere;
    max-height: 50vh;
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
