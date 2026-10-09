<script lang="ts">
  import { onMount } from 'svelte';
  import ProgressBar from '../components/ProgressBar.svelte';
  import { api } from '../lib/api';
  import { formatBytes } from '../lib/format';
  import type { CatalogEntry, ModelsResponse } from '../lib/types';
  import { server } from '../stores/server.svelte';

  let data = $state<ModelsResponse | null>(null);
  let error = $state('');
  let actionError = $state('');
  let pending = $state<Record<string, boolean>>({});

  let loadedPath = $derived(server.info?.model?.path ?? null);
  let loading = $derived(server.info?.state === 'loading');

  async function refresh() {
    try {
      data = await api.models();
      error = '';
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
    }
  }

  onMount(refresh);

  // A download that ends (done, error or cancelled) changes the local list and the statuses.
  let seenFinished = server.downloadsFinished;
  $effect(() => {
    if (server.downloadsFinished !== seenFinished) {
      seenFinished = server.downloadsFinished;
      refresh();
    }
  });

  async function act(key: string, fn: () => Promise<unknown>) {
    pending = { ...pending, [key]: true };
    actionError = '';
    try {
      await fn();
    } catch (e) {
      actionError = e instanceof Error ? e.message : String(e);
    } finally {
      pending = { ...pending, [key]: false };
    }
  }

  function download(entry: CatalogEntry) {
    act(`dl:${entry.id}`, async () => {
      await api.download(entry.id);
      await refresh();
    });
  }

  function isDownloading(entry: CatalogEntry): boolean {
    const d = server.downloads[entry.id];
    return d ? d.state === 'running' : entry.status === 'downloading';
  }

  const statusLabel: Record<string, string> = { on_disk: 'On disk', downloading: 'Downloading', available: 'Available' };
</script>

<h1>Models</h1>

{#if error}
  <p class="banner error" role="alert">Could not list models: {error} <button type="button" onclick={refresh}>Retry</button></p>
{/if}
{#if actionError}
  <p class="banner error" role="alert">{actionError}</p>
{/if}
{#if server.info?.state === 'error'}
  <p class="banner error">Last load failed: {server.info.error}</p>
{/if}

{#if server.info?.model}
  <div class="banner info">
    <span>Loaded: <strong>{server.info.model.name}</strong> ({server.info.model.arch}, {server.info.model.load_s.toFixed(1)} s load)</span>
    <button type="button" onclick={() => act('unload', api.unload)} disabled={pending.unload}>Unload</button>
  </div>
{:else if loading}
  <p class="banner warn">Loading a model...</p>
{/if}

{#if data}
  <p class="muted">Models directory: <code>{data.models_dir}</code></p>

  <h2>On this machine</h2>
  {#if data.local.length === 0}
    <p class="muted">No gguf files in the models directory yet. Download one from the catalog below.</p>
  {:else}
    <div class="table-wrap">
      <table>
        <thead>
          <tr><th>Model</th><th>Size</th><th>Arch</th><th>Status</th><th><span class="visually-hidden">Actions</span></th></tr>
        </thead>
        <tbody>
          {#each data.local as m (m.path)}
            <tr class:loaded={m.path === loadedPath}>
              <td class="name">
                {m.name}
                {#if m.path === loadedPath}<span class="badge ok">loaded</span>{/if}
                {#if m.shards > 1}<span class="muted"> ({m.shards} shards)</span>{/if}
              </td>
              <td>{formatBytes(m.bytes)}</td>
              <td>{m.arch || '?'}</td>
              <td class="tags">
                {#if m.streamable}
                  <span class="badge ok">streamable</span>
                {:else}
                  <span class="badge warn" title="This build does not stream this architecture: it runs as plain mmap">not streamable</span>
                {/if}
                {#if !m.complete}<span class="badge err">incomplete</span>{/if}
              </td>
              <td>
                <button
                  type="button"
                  class:primary={m.path !== loadedPath}
                  disabled={!m.complete || loading || pending[`load:${m.path}`]}
                  onclick={() => act(`load:${m.path}`, () => api.load(m.path))}
                >
                  {m.path === loadedPath ? 'Reload' : 'Load'}<span class="visually-hidden"> {m.name}</span>
                </button>
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
  {/if}

  <h2>Catalog</h2>
  <div class="catalog">
    {#each data.catalog as entry (entry.id)}
      {@const dl = server.downloads[entry.id]}
      <article class="card entry">
        <header>
          <h3>{entry.title}</h3>
          <span class="badge">{entry.quant}</span>
          <span class="badge {entry.status === 'on_disk' ? 'ok' : ''}">{statusLabel[entry.status] ?? entry.status}</span>
        </header>
        <p class="muted size">{formatBytes(entry.bytes)}, <code>{entry.file}</code></p>
        {#if entry.blurb}<p>{entry.blurb}</p>{/if}
        {#if entry.note}<p class="muted note">{entry.note}</p>{/if}
        {#if dl && (dl.state === 'running' || isDownloading(entry))}
          <ProgressBar received={dl.received} total={dl.total} label="Download progress for {entry.title}" />
        {/if}
        {#if dl?.state === 'error'}
          <p class="err">Download failed: {dl.error}</p>
        {:else if dl?.state === 'cancelled'}
          <p class="muted">Download cancelled. Downloading again resumes it.</p>
        {/if}
        <div class="actions">
          {#if isDownloading(entry)}
            <button type="button" class="danger" disabled={pending[`cancel:${entry.id}`]} onclick={() => act(`cancel:${entry.id}`, () => api.cancelDownload(entry.id))}>
              Cancel<span class="visually-hidden"> download of {entry.title}</span>
            </button>
          {:else if entry.status !== 'on_disk'}
            <button type="button" class="primary" disabled={pending[`dl:${entry.id}`]} onclick={() => download(entry)}>
              Download<span class="visually-hidden"> {entry.title}</span>
            </button>
          {/if}
        </div>
      </article>
    {/each}
  </div>
{:else if !error}
  <p class="muted">Loading...</p>
{/if}

<style>
  .name {
    overflow-wrap: anywhere;
  }
  /* Every cell stays a table cell and centred, whatever its content lays out as. */
  td,
  td.tags {
    display: table-cell;
    vertical-align: middle;
  }
  tr.loaded {
    background: var(--ok-bg);
  }
  .tags {
    display: flex;
    gap: 0.3rem;
    flex-wrap: wrap;
  }
  .catalog {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(min(100%, 300px), 1fr));
    gap: 0.75rem;
  }
  .entry header {
    display: flex;
    align-items: center;
    gap: 0.4rem;
    flex-wrap: wrap;
  }
  .entry h3 {
    margin: 0;
    font-size: 1rem;
    margin-right: auto;
  }
  .entry p {
    margin: 0.4rem 0;
  }
  .size {
    font-size: 0.85rem;
    overflow-wrap: anywhere;
  }
  .note {
    font-size: 0.85rem;
  }
  .err {
    color: var(--err);
    font-size: 0.85rem;
  }
  .actions {
    margin-top: 0.5rem;
    display: flex;
    gap: 0.5rem;
  }
</style>
