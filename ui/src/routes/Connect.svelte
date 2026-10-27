<script lang="ts">
  // Connect apps (docs/agent.md, "Connecting other apps"): every coding agent or app the server can
  // serve, with the exact settings for this server. For a client whose settings live in a file the
  // server can write them, keeping a backup of the file it replaces.
  import { onMount } from 'svelte';
  import Icon from '../components/Icon.svelte';
  import { ApiError } from '../lib/api';
  import type { ConnectClient, ConnectWritten } from '../lib/agent';
  import { agentApi } from '../lib/agentApi';
  import { t } from '../stores/prefs.svelte';

  let clients = $state<ConnectClient[] | null>(null);
  let error = $state('');
  let copied = $state('');
  let writing = $state('');
  let written = $state<Record<string, ConnectWritten>>({});
  let writeError = $state<Record<string, string>>({});
  let confirm = $state('');

  async function load() {
    try {
      clients = await agentApi.connect();
      error = '';
    } catch (e) {
      error = e instanceof ApiError && e.status === 404 ? t('connect.unavailable') : e instanceof Error ? e.message : String(e);
    }
  }
  onMount(load);

  async function copy(c: ConnectClient) {
    try {
      await navigator.clipboard.writeText(c.snippet);
      copied = c.id;
      setTimeout(() => (copied = copied === c.id ? '' : copied), 1500);
    } catch {
      copied = '';
    }
  }

  async function write(c: ConnectClient) {
    if (confirm !== c.id) {
      confirm = c.id;
      return;
    }
    confirm = '';
    writing = c.id;
    writeError = { ...writeError, [c.id]: '' };
    try {
      written = { ...written, [c.id]: await agentApi.writeConnect(c.id) };
    } catch (e) {
      const msg = e instanceof ApiError && e.code === 'local_only' ? t('error.localOnly') : e instanceof Error ? e.message : String(e);
      writeError = { ...writeError, [c.id]: msg };
    } finally {
      writing = '';
    }
  }
</script>

<div class="page-col">
  <div class="page-head">
    <h1>{t('nav.connect')}</h1>
    <p>{t('connect.lead')}</p>
  </div>

  {#if error}<p class="banner error" role="alert">{error} <button type="button" class="small" onclick={load}>{t('action.retry')}</button></p>{/if}

  {#if clients === null && !error}
    <div class="grid">{#each [0, 1, 2, 3] as i (i)}<span class="skeleton card-skel"></span>{/each}</div>
  {:else if clients && clients.length === 0}
    <div class="empty-state card"><Icon name="plug" size={28} /><h2>{t('connect.none')}</h2></div>
  {:else if clients}
    <div class="grid">
      {#each clients as c (c.id)}
        <section class="card client" aria-labelledby="cl-{c.id}">
          <h2 id="cl-{c.id}">{c.name}</h2>
          <p class="muted small">{c.how}</p>
          {#if c.file}<p class="small">{t('connect.file')} <code>{c.file}</code></p>{/if}
          <div class="snippet">
            <pre>{c.snippet}</pre>
            <button type="button" class="small copy" onclick={() => copy(c)}><Icon name={copied === c.id ? 'check' : 'copy'} size={14} />{copied === c.id ? t('action.copied') : t('action.copy')}</button>
          </div>
          {#if c.writable}
            <div class="row wrap">
              {#if confirm === c.id}
                <span class="small">{t('connect.write.confirm', { file: c.file ?? '' })}</span>
                <button type="button" class="primary small" onclick={() => write(c)}>{t('connect.write')}</button>
                <button type="button" class="small" onclick={() => (confirm = '')}>{t('action.cancel')}</button>
              {:else}
                <button type="button" class="primary" disabled={writing === c.id} onclick={() => write(c)}>{t('connect.write')}</button>
              {/if}
            </div>
          {/if}
          {#if written[c.id]}
            <p class="banner ok small" role="status">
              {t('connect.written', { file: written[c.id].file })}
              {#if written[c.id].backup}<br />{t('connect.backup', { file: written[c.id].backup ?? '' })}{/if}
            </p>
          {/if}
          {#if writeError[c.id]}<p class="banner error small" role="alert">{writeError[c.id]}</p>{/if}
        </section>
      {/each}
    </div>
  {/if}
</div>

<style>
  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(min(100%, 22rem), 1fr));
    gap: var(--s3);
  }
  .card-skel {
    height: 12rem;
    border-radius: var(--radius);
  }
  .client {
    display: flex;
    flex-direction: column;
    gap: var(--s2);
    min-width: 0;
  }
  .client h2 {
    margin: 0;
    font-size: 1rem;
  }
  .client p {
    margin: 0;
  }
  .snippet {
    position: relative;
  }
  .snippet pre {
    margin: 0;
    padding-right: 5.5rem;
    font-size: 0.75rem;
    max-height: 14rem;
    white-space: pre-wrap;
    overflow-wrap: anywhere;
  }
  .copy {
    position: absolute;
    top: var(--s2);
    right: var(--s2);
  }
  .wrap {
    flex-wrap: wrap;
  }
  code {
    overflow-wrap: anywhere;
  }
  .banner {
    margin: 0;
    display: block;
  }
</style>
