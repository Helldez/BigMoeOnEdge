<script lang="ts">
  // Saved connections to an OpenAI-compatible chat service (docs/agent.md, "Providers"). The key
  // is written once and never read back: the form says whether one is saved and leaves it as it is
  // unless a new one is typed.
  import Dialog from '../Dialog.svelte';
  import Icon from '../Icon.svelte';
  import type { Provider, ProviderDraft } from '../../lib/agent';
  import { agentApi } from '../../lib/agentApi';
  import { agent } from '../../stores/agent.svelte';
  import { t, tp } from '../../lib/text';

  let editing = $state<Provider | null>(null);
  let open = $state(false);
  let name = $state('');
  let baseUrl = $state('');
  let models = $state('');
  let key = $state('');
  let error = $state('');
  let saving = $state(false);
  let confirm = $state('');
  let listError = $state('');

  function start(p: Provider | null) {
    editing = p;
    name = p?.name ?? '';
    baseUrl = p?.base_url ?? '';
    models = (p?.models ?? []).join('\n');
    key = '';
    error = '';
    open = true;
  }

  let modelList = $derived(models.split(/[\n,]/).map((m) => m.trim()).filter(Boolean));
  let valid = $derived(name.trim() !== '' && /^https?:\/\/\S+$/.test(baseUrl.trim()) && modelList.length > 0);

  async function save(e: SubmitEvent) {
    e.preventDefault();
    if (!valid) return;
    saving = true;
    error = '';
    const body: ProviderDraft = { name: name.trim(), base_url: baseUrl.trim(), models: modelList };
    if (key) body.api_key = key;
    try {
      if (editing) await agentApi.updateProvider(editing.id, body);
      else await agentApi.createProvider(body);
      open = false;
      await agent.refreshProviders();
    } catch (err) {
      error = err instanceof Error ? err.message : String(err);
    } finally {
      saving = false;
      key = '';
    }
  }

  async function remove(p: Provider) {
    if (confirm !== p.id) {
      confirm = p.id;
      return;
    }
    confirm = '';
    try {
      await agentApi.deleteProvider(p.id);
      await agent.refreshProviders();
    } catch (err) {
      listError = err instanceof Error ? err.message : String(err);
    }
  }
</script>

<section class="card section" aria-labelledby="providers-title">
  <div class="row head">
    <h2 id="providers-title">{t('providers.title')}</h2>
    <span class="spacer"></span>
    <button type="button" onclick={() => start(null)}><Icon name="plus" size={16} />{t('providers.add')}</button>
  </div>
  <p class="muted small">{t('providers.help')}</p>
  {#if listError}<p class="banner error" role="alert">{listError}</p>{/if}
  {#if agent.providers.length === 0}
    <p class="empty muted">{t('providers.none')}</p>
  {:else}
    <ul class="list">
      {#each agent.providers as p (p.id)}
        <li class="setting">
          <div class="what">
            <span class="label">{p.name}</span>
            <span class="muted small mono">{p.base_url}</span>
            <span class="muted small">{tp('providers.models', p.models.length)} · {p.has_key ? t('providers.keySaved') : t('providers.noKey')}</span>
          </div>
          <div class="row">
            {#if confirm === p.id}
              <span class="small">{t('providers.remove.confirm')}</span>
              <button type="button" class="danger small" onclick={() => remove(p)}>{t('action.remove')}</button>
              <button type="button" class="small" onclick={() => (confirm = '')}>{t('action.keep')}</button>
            {:else}
              <button type="button" class="small" onclick={() => start(p)}>{t('action.edit')}</button>
              <button type="button" class="small danger" onclick={() => remove(p)}>{t('action.remove')}</button>
            {/if}
          </div>
        </li>
      {/each}
    </ul>
  {/if}
</section>

<Dialog bind:open title={editing ? t('providers.edit') : t('providers.add')}>
  <form id="provider-form" onsubmit={save}>
    <div class="field-row">
      <label for="pv-name">{t('providers.name')}</label>
      <input id="pv-name" type="text" bind:value={name} autocomplete="off" />
    </div>
    <div class="field-row">
      <label for="pv-url">{t('providers.url')}</label>
      <input id="pv-url" type="url" class="mono" placeholder="https://api.example.com/v1" bind:value={baseUrl} autocomplete="off" spellcheck="false" />
      <p class="hint">{t('providers.url.help')}</p>
    </div>
    <div class="field-row">
      <label for="pv-models">{t('providers.modelNames')}</label>
      <textarea id="pv-models" rows="3" class="mono" bind:value={models} spellcheck="false"></textarea>
      <p class="hint">{t('providers.modelNames.help')}</p>
    </div>
    <div class="field-row">
      <label for="pv-key">{t('providers.key')}</label>
      <input id="pv-key" type="password" bind:value={key} autocomplete="new-password" placeholder={editing?.has_key ? t('providers.key.keep') : ''} />
      <p class="hint">{t('providers.key.help')}</p>
    </div>
    {#if error}<p class="err-text small" role="alert">{error}</p>{/if}
  </form>
  {#snippet actions()}
    <button type="button" onclick={() => (open = false)}>{t('action.cancel')}</button>
    <button type="submit" form="provider-form" class="primary" disabled={!valid || saving}>{t('action.save')}</button>
  {/snippet}
</Dialog>

<style>
  .head h2 {
    margin: 0;
  }
  .list {
    list-style: none;
    margin: var(--s2) 0 0;
    padding: 0;
  }
  .mono {
    font-family: var(--mono);
    overflow-wrap: anywhere;
  }
  .empty {
    margin: var(--s3) 0 0;
  }
</style>
