<script lang="ts">
  import { onMount } from 'svelte';
  import HuggingFace from '../components/models/HuggingFace.svelte';
  import PlanProgress from '../components/plan/PlanProgress.svelte';
  import ProgressBar from '../components/ProgressBar.svelte';
  import { api } from '../lib/api';
  import { formatBytes, resumeLabel } from '../lib/format';
  import { roleLabel, splitByRole } from '../lib/roles';
  import type { CatalogEntry, ModelsResponse } from '../lib/types';
  import { server } from '../stores/server.svelte';

  let data = $state<ModelsResponse | null>(null);
  let error = $state('');
  let actionError = $state('');
  let pending = $state<Record<string, boolean>>({});

  let loadedPath = $derived(server.info?.model?.path ?? null);
  // Projectors and draft heads (by their headers) are no model to load: listed apart, after the models.
  let localGroups = $derived(splitByRole(data?.local ?? []));
  let planning = $derived(server.info?.state === 'planning');
  let loading = $derived(server.info?.state === 'loading' || planning);

  // Seconds since the load began, so a long load reads as progress rather than as a hang.
  let elapsed = $state(0);
  $effect(() => {
    if (!loading) {
      elapsed = 0;
      return;
    }
    const t0 = Date.now();
    const id = setInterval(() => (elapsed = Math.round((Date.now() - t0) / 1000)), 1000);
    return () => clearInterval(id);
  });

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

  // A model added from a repository is a catalog entry like the others; it can be forgotten
  // (its files stay) and, right after it is added, downloaded in the same breath.
  async function added(id: string) {
    await refresh();
    await act(`dl:${id}`, async () => {
      await api.download(id);
      await refresh();
    });
  }

  function deletePartial(entry: CatalogEntry) {
    act(`part:${entry.id}`, async () => {
      await api.deletePartial(entry.id);
      server.forgetDownload(entry.id);
      await refresh();
    });
  }

  function remove(entry: CatalogEntry) {
    act(`rm:${entry.id}`, async () => {
      await api.removeAdded(entry.id);
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
  <p class="banner warn" role="status">{planning ? 'Measuring this machine, then loading' : 'Loading the model'} · {elapsed} s</p>
  {#if planning}<PlanProgress progress={server.planProgress} />{/if}
{/if}

{#if data}
  <p class="muted">
    Models directory: <code>{data.models_dir}</code>
    {#if data.scan_dirs?.length}· also listed: {#each data.scan_dirs as d, i (d)}{i ? ', ' : ''}<code>{d}</code>{/each}{/if}
  </p>

  <h2>On this machine</h2>
  {#if data.local.length === 0}
    <p class="muted">No gguf files in the models directory yet. Get one below: from a Hugging Face repository, or from the catalog.</p>
  {:else}
    <div class="table-wrap">
      <table>
        <thead>
          <tr><th>Model</th><th>Size</th><th>Arch</th><th>Status</th><th><span class="visually-hidden">Actions</span></th></tr>
        </thead>
        <tbody>
          {#each localGroups.models as m (m.path)}
            <tr class:loaded={m.path === loadedPath}>
              <td class="name">
                {m.name}
                {#if m.path === loadedPath}<span class="badge ok">loaded</span>{/if}
                {#if m.shards > 1}<span class="muted"> ({m.shards} shards)</span>{/if}
                {#if m.dir && m.dir !== data.models_dir}<span class="muted"> in <code>{m.dir}</code></span>{/if}
                {#if m.folder}<span class="muted"> / <code>{m.folder}</code></span>{/if}
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
    {#if localGroups.companions.length}
      <details class="companions">
        <summary class="muted">{localGroups.companions.length} companion {localGroups.companions.length === 1 ? 'file' : 'files'}: not models to load, by their headers</summary>
        <ul>
          {#each localGroups.companions as m (m.path)}
            <li>
              <span class="name">{m.name}</span>
              <span class="badge">{roleLabel(m.role ?? '')}</span>
              <span class="muted">{formatBytes(m.bytes)}{m.arch ? `, ${m.arch}` : ''}{m.folder ? `, in ${m.folder}` : ''}</span>
            </li>
          {/each}
        </ul>
      </details>
    {/if}
  {/if}

  <h2>Get a model</h2>
  <HuggingFace onAdded={added} />

  <h2>Catalog</h2>
  <div class="catalog">
    {#each data.catalog as entry (entry.id)}
      {@const dl = server.downloads[entry.id]}
      <article class="card entry">
        <header>
          <h3>{entry.title}</h3>
          {#if entry.quant}<span class="badge">{entry.quant}</span>{/if}
          {#if entry.added}<span class="badge" title="Added from a repository">added</span>{/if}
          <span class="badge {entry.status === 'on_disk' ? 'ok' : ''}">{statusLabel[entry.status] ?? entry.status}</span>
        </header>
        <p class="muted size">{formatBytes(entry.bytes)}, <code>{entry.file}</code></p>
        {#if entry.blurb}<p>{entry.blurb}</p>{/if}
        {#if entry.note}<p class="muted note">{entry.note}</p>{/if}
        {#if dl && (dl.state === 'running' || isDownloading(entry))}
          <ProgressBar received={dl.received} total={dl.total} label="Download progress for {entry.title}" />
          {#if dl.state === 'running' && dl.error}<p class="muted note">{dl.error}</p>{/if}
        {:else if entry.partial}
          <ProgressBar received={entry.received ?? 0} total={entry.bytes} label="Downloaded so far of {entry.title}" />
        {/if}
        {#if dl?.state === 'error'}
          <p class="err">Download failed: {dl.error}</p>
        {:else if dl?.state === 'cancelled'}
          <p class="muted">Download cancelled. The partial file is kept: Resume continues it.</p>
        {/if}
        <div class="actions">
          {#if isDownloading(entry)}
            <button type="button" class="danger" disabled={pending[`cancel:${entry.id}`]} onclick={() => act(`cancel:${entry.id}`, () => api.cancelDownload(entry.id))}>
              Cancel<span class="visually-hidden"> download of {entry.title}</span>
            </button>
          {:else if entry.status !== 'on_disk'}
            <button type="button" class="primary" disabled={pending[`dl:${entry.id}`]} onclick={() => download(entry)}>
              {entry.partial ? resumeLabel(entry.received ?? 0, entry.bytes) : 'Download'}<span class="visually-hidden"> {entry.title}</span>
            </button>
            {#if entry.partial}
              <button type="button" class="danger" disabled={pending[`part:${entry.id}`]} onclick={() => deletePartial(entry)} title="Delete the partially downloaded file and free its space">
                Delete partial download<span class="visually-hidden"> of {entry.title}</span>
              </button>
            {/if}
          {/if}
          {#if entry.added && !isDownloading(entry)}
            <button type="button" disabled={pending[`rm:${entry.id}`]} onclick={() => remove(entry)} title="Forget this entry; its files stay">
              Remove<span class="visually-hidden"> {entry.title} from the catalog</span>
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
  .companions {
    margin-top: 0.5rem;
  }
  .companions summary {
    cursor: pointer;
  }
  .companions ul {
    list-style: none;
    padding: 0;
    margin: 0.4rem 0 0;
    display: flex;
    flex-direction: column;
    gap: 0.25rem;
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
    flex-wrap: wrap;
    gap: 0.5rem;
  }
</style>
