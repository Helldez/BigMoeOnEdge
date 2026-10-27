<script lang="ts">
  // Skills (docs/agent.md, "Skills"): instructions the agent reads when a job calls for them. Each
  // can be switched off; a person's own can be written, changed and removed here.
  import { onMount } from 'svelte';
  import Dialog from '../components/Dialog.svelte';
  import Icon from '../components/Icon.svelte';
  import { ApiError } from '../lib/api';
  import type { Skill } from '../lib/agent';
  import { agentApi } from '../lib/agentApi';
  import { t } from '../stores/prefs.svelte';

  let skills = $state<Skill[] | null>(null);
  let error = $state('');
  let actionError = $state('');
  let open = $state(false);
  let editing = $state<Skill | null>(null);
  let name = $state('');
  let description = $state('');
  let body = $state('');
  let formError = $state('');
  let saving = $state(false);
  let confirm = $state('');
  let query = $state('');

  const NAME = /^[a-z0-9]+(-[a-z0-9]+)*$/;

  async function load() {
    try {
      skills = await agentApi.skills();
      error = '';
    } catch (e) {
      error = e instanceof ApiError && e.status === 404 ? t('skills.unavailable') : e instanceof Error ? e.message : String(e);
    }
  }
  onMount(load);

  function start(s: Skill | null) {
    editing = s;
    name = s?.name ?? '';
    description = s?.description ?? '';
    body = s?.body ?? '';
    formError = '';
    open = true;
  }

  let nameOk = $derived(NAME.test(name));
  let valid = $derived(nameOk && description.trim() !== '' && body.trim() !== '');

  async function save(e: SubmitEvent) {
    e.preventDefault();
    if (!valid) return;
    saving = true;
    formError = '';
    const s = { name, description: description.trim(), body };
    try {
      if (editing) await agentApi.updateSkill(editing.name, s);
      else await agentApi.createSkill(s);
      open = false;
      await load();
    } catch (err) {
      formError = err instanceof Error ? err.message : String(err);
    } finally {
      saving = false;
    }
  }

  async function toggle(s: Skill, enabled: boolean) {
    actionError = '';
    skills = (skills ?? []).map((x) => (x.name === s.name ? { ...x, enabled } : x));
    try {
      await agentApi.enableSkill(s.name, enabled);
    } catch (e) {
      actionError = e instanceof Error ? e.message : String(e);
      await load();
    }
  }

  async function remove(s: Skill) {
    if (confirm !== s.name) {
      confirm = s.name;
      return;
    }
    confirm = '';
    try {
      await agentApi.deleteSkill(s.name);
      await load();
    } catch (e) {
      actionError = e instanceof Error ? e.message : String(e);
    }
  }

  let shown = $derived(
    (skills ?? []).filter((s) => {
      const q = query.trim().toLowerCase();
      return !q || s.name.includes(q) || s.description.toLowerCase().includes(q) || (s.title ?? '').toLowerCase().includes(q);
    }),
  );
  const tone: Record<string, string> = { bundled: '', user: 'info', workspace: 'ok' };
</script>

<div class="page-col">
  <div class="page-head">
    <h1>{t('nav.skills')}</h1>
    <button type="button" class="primary" onclick={() => start(null)}><Icon name="plus" size={16} />{t('skills.new')}</button>
    <p>{t('skills.lead')}</p>
  </div>

  {#if error}
    <p class="banner error" role="alert">{error} <button type="button" class="small" onclick={load}>{t('action.retry')}</button></p>
  {/if}
  {#if actionError}<p class="banner error" role="alert">{actionError}</p>{/if}

  {#if skills === null && !error}
    <div class="list">{#each [0, 1, 2] as i (i)}<span class="skeleton row-skel"></span>{/each}</div>
  {:else if skills && skills.length === 0}
    <div class="empty-state card">
      <Icon name="spark" size={28} />
      <h2>{t('skills.none')}</h2>
      <p>{t('skills.none.help')}</p>
      <button type="button" class="primary" onclick={() => start(null)}>{t('skills.new')}</button>
    </div>
  {:else if skills}
    {#if skills.length > 6}
      <input class="search" type="search" placeholder={t('skills.search')} aria-label={t('skills.search')} bind:value={query} />
    {/if}
    <ul class="list">
      {#each shown as s (s.name)}
        <li class="card skill" class:off={!s.enabled}>
          <div class="what">
            <div class="row">
              <strong>{s.title || s.name}</strong>
              <span class="badge {tone[s.source] ?? ''}">{t(`skills.source.${s.source}`)}</span>
            </div>
            <p class="muted small">{s.description}</p>
            <code class="small">{s.name}</code>
          </div>
          <div class="side">
            <label class="row small">
              <input type="checkbox" class="switch" role="switch" checked={s.enabled} onchange={(e) => toggle(s, e.currentTarget.checked)} />
              <span>{s.enabled ? t('skills.on') : t('skills.off')}</span>
            </label>
            {#if s.source === 'user'}
              <div class="row">
                {#if confirm === s.name}
                  <button type="button" class="small danger" onclick={() => remove(s)}>{t('action.delete')}</button>
                  <button type="button" class="small" onclick={() => (confirm = '')}>{t('action.keep')}</button>
                {:else}
                  <button type="button" class="small" onclick={() => start(s)}>{t('action.edit')}</button>
                  <button type="button" class="small danger" onclick={() => remove(s)}>{t('action.delete')}</button>
                {/if}
              </div>
            {:else}
              <button type="button" class="small ghost" onclick={() => start(s)}>{t('skills.view')}</button>
            {/if}
          </div>
        </li>
      {/each}
    </ul>
  {/if}
</div>

<Dialog bind:open wide title={editing ? (editing.source === 'user' ? t('skills.edit') : editing.title || editing.name) : t('skills.new')}>
  <form id="skill-form" onsubmit={save}>
    <fieldset disabled={!!editing && editing.source !== 'user'}>
      <div class="field-row">
        <label for="sk-name">{t('skills.name')}</label>
        <input id="sk-name" type="text" class="mono" bind:value={name} autocomplete="off" spellcheck="false" placeholder="release-notes" />
        <p class="hint" class:err-text={name !== '' && !nameOk}>{t('skills.name.help')}</p>
      </div>
      <div class="field-row">
        <label for="sk-desc">{t('skills.description')}</label>
        <input id="sk-desc" type="text" bind:value={description} />
        <p class="hint">{t('skills.description.help')}</p>
      </div>
      <div class="field-row">
        <label for="sk-body">{t('skills.instructions')}</label>
        <textarea id="sk-body" rows="12" class="mono" bind:value={body}></textarea>
        <p class="hint">{t('skills.instructions.help')}</p>
      </div>
    </fieldset>
    {#if formError}<p class="err-text small" role="alert">{formError}</p>{/if}
  </form>
  {#snippet actions()}
    <button type="button" onclick={() => (open = false)}>{editing && editing.source !== 'user' ? t('action.close') : t('action.cancel')}</button>
    {#if !editing || editing.source === 'user'}
      <button type="submit" form="skill-form" class="primary" disabled={!valid || saving}>{t('action.save')}</button>
    {/if}
  {/snippet}
</Dialog>

<style>
  .list {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: var(--s2);
  }
  .row-skel {
    height: 5rem;
    border-radius: var(--radius);
  }
  .search {
    width: 100%;
    margin-bottom: var(--s3);
  }
  .skill {
    display: flex;
    gap: var(--s4);
    align-items: flex-start;
    flex-wrap: wrap;
  }
  .skill.off .what {
    color: var(--muted);
  }
  .what {
    flex: 1 1 20rem;
    min-width: 0;
  }
  .what p {
    margin: var(--s1) 0;
  }
  .side {
    display: flex;
    flex-direction: column;
    align-items: flex-end;
    gap: var(--s2);
  }
  fieldset {
    border: none;
    padding: 0;
    margin: 0;
  }
  .mono {
    font-family: var(--mono);
  }
  .empty-state :global(svg) {
    color: var(--accent);
  }
</style>
