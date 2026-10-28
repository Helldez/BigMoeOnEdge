<script lang="ts">
  import { onMount } from 'svelte';
  import HuggingFace from '../components/models/HuggingFace.svelte';
  import UrlImport from '../components/models/UrlImport.svelte';
  import PlanProgress from '../components/plan/PlanProgress.svelte';
  import ProgressBar from '../components/ProgressBar.svelte';
  import StreamingBadge from '../components/StreamingBadge.svelte';
  import { api } from '../lib/api';
  import { formatBytes, resumeLabel } from '../lib/format';
  import { libraryRows, type LibraryRow, type RowState } from '../lib/library';
  import { href } from '../lib/route';
  import { roleLabel, splitByRole } from '../lib/roles';
  import type { CatalogEntry, ModelsResponse } from '../lib/types';
  import { server } from '../stores/server.svelte';

  let data = $state<ModelsResponse | null>(null);
  let error = $state('');
  let actionError = $state('');
  let pending = $state<Record<string, boolean>>({});
  // The row a delete waits on a second press for, by key.
  let confirming = $state('');
  // The entry the person just started: its row is scrolled to, now and again when it lands on disk.
  let focusId = $state('');

  let loadedPath = $derived(server.info?.model?.path ?? null);
  // Projectors and draft heads (by their headers) are no model to load: listed apart, after the models.
  let localGroups = $derived(splitByRole(data?.local ?? []));
  let rows = $derived(data ? libraryRows(data, localGroups.models, server.downloads, loadedPath) : []);
  let yours = $derived(rows.filter((r) => !r.suggested));
  let suggested = $derived(rows.filter((r) => r.suggested));
  // Nothing here and nothing coming: the screen is the three steps to a first model.
  let empty = $derived(!!data && yours.length === 0 && localGroups.companions.length === 0);
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
    focusId = entry.id;
    act(`dl:${entry.id}`, async () => {
      await api.download(entry.id);
      await refresh();
    });
  }

  // A model added from a repository or a link is a catalog entry like the others, downloaded in
  // the same breath, and its row is where the person follows it. A download that does not start
  // throws back to the card that asked, which shows why where the person is looking.
  async function added(id: string) {
    focusId = id;
    await refresh();
    pending = { ...pending, [`dl:${id}`]: true };
    try {
      await api.download(id);
    } finally {
      pending = { ...pending, [`dl:${id}`]: false };
      await refresh();
    }
  }

  function deletePartial(entry: CatalogEntry) {
    act(`part:${entry.id}`, async () => {
      await api.deletePartial(entry.id);
      server.forgetDownload(entry.id);
      await refresh();
    });
  }

  function forget(entry: CatalogEntry) {
    act(`rm:${entry.id}`, async () => {
      await api.removeAdded(entry.id);
      await refresh();
    });
  }

  function deleteModel(row: LibraryRow) {
    const name = row.deleteName;
    if (!name) return;
    if (confirming !== row.key) {
      confirming = row.key;
      return;
    }
    confirming = '';
    act(`del:${row.key}`, async () => {
      await api.deleteModel(name);
      if (row.entry) server.forgetDownload(row.entry.id);
      await refresh();
    });
  }

  function load(path: string) {
    act(`load:${path}`, () => api.load(path));
  }

  // Scrolls a row into view when it is the one being followed (an action, so it runs again when the
  // row is replaced: a finished download moves from the catalog's row to the file's).
  function follow(node: HTMLElement, on: boolean) {
    const go = (v: boolean) => {
      if (v) node.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
    };
    go(on);
    return { update: go };
  }

  const stateLabel: Record<RowState, string> = {
    available: 'Available',
    downloading: 'Downloading',
    partial: 'Partly downloaded',
    on_disk: 'On this machine',
    loaded: 'Loaded',
    incomplete: 'Shards missing',
  };
  const stateClass: Record<RowState, string> = {
    available: '',
    downloading: 'info',
    partial: 'warn',
    on_disk: 'ok',
    loaded: 'ok',
    incomplete: 'err',
  };
  import { t } from '../lib/text';
</script>

<div class="page-col">
<div class="page-head">
  <h1>{t('nav.models')}</h1>
  <p>{t('models.lead')}</p>
</div>

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
    <span>{t('models.loaded')} <strong>{server.info.model.name}</strong> ({server.info.model.arch}, {t('models.loadTime', { s: server.info.model.load_s.toFixed(1) })})</span>
    <StreamingBadge streaming={server.info.model.streaming} />
    {#if !rows.some((r) => r.state === 'loaded')}
      <!-- A model opened from outside the listed folders has no row to carry these. -->
      <button type="button" onclick={() => act('unload', api.unload)} disabled={pending.unload}>Unload</button>
      <a class="button primary" href={href('chat')}>Go to Chat</a>
    {/if}
  </div>
{:else if loading}
  <p class="banner warn" role="status">{planning ? 'Measuring this machine, then loading' : 'Loading the model'} · {elapsed} s</p>
  {#if planning}<PlanProgress progress={server.planProgress} />{/if}
{/if}

{#snippet row(r: LibraryRow)}
  {@const e = r.entry}
  {@const m = r.local}
  {@const dl = e ? server.downloads[e.id] : undefined}
  {@const size = m ? m.bytes : (e?.bytes ?? 0)}
  <li class="row" class:loaded={r.state === 'loaded'} class:followed={!!e && e.id === focusId} use:follow={!!e && e.id === focusId}>
    <div class="what">
      <div class="title">
        <strong class="name">{r.title}</strong>
        {#if e?.quant}<span class="badge">{e.quant}</span>{/if}
        <span class="badge {stateClass[r.state]}">{stateLabel[r.state]}</span>
        {#if m}
          {#if m.streamable}
            <span class="badge ok">streamable</span>
          {:else}
            <span class="badge warn" title="This build does not stream this architecture: it runs as plain mmap">not streamable</span>
          {/if}
        {/if}
        {#if e?.added}<span class="badge" title="Added from a repository or a link">added</span>{/if}
      </div>
      <p class="muted meta">
        {formatBytes(size)}{#if m?.arch}, {m.arch}{/if}{#if m && m.shards > 1}, {m.shards} shards{:else if e?.note}, {e.note}{/if}
        {#if m && m.name !== r.title}· <code>{m.name}</code>{:else if !m && e}· <code>{e.file}</code>{/if}
        {#if m?.folder}· in <code>{m.folder}</code>{/if}
        {#if m && data && m.dir !== data.models_dir}· in <code>{m.dir}</code>{/if}
      </p>
      {#if e?.blurb && !m}<p class="blurb">{e.blurb}</p>{/if}
      {#if r.state === 'downloading'}
        <ProgressBar received={dl?.received ?? e?.received ?? 0} total={dl?.total || e?.bytes || 0} label="Download progress for {r.title}" />
        {#if dl?.state === 'running' && dl.error}<p class="muted note">{dl.error}</p>{/if}
      {:else if r.state === 'partial' && e}
        <ProgressBar received={e.received ?? 0} total={e.bytes} label="Downloaded so far of {r.title}" />
      {/if}
      {#if dl?.state === 'error'}
        <p class="err">Download failed: {dl.error}</p>
      {:else if dl?.state === 'cancelled' && r.state !== 'available'}
        <p class="muted note">Download cancelled. The partial file is kept: Resume continues it.</p>
      {/if}
      {#if confirming === r.key && m}
        <p class="err" role="alert">
          Delete {m.files.length === 1 ? 'this file' : `all ${m.files.length} files`} ({formatBytes(m.bytes)}) from the disk? Press Delete again to confirm.
        </p>
      {/if}
    </div>
    <div class="actions">
      {#if r.state === 'loaded' && m}
        <button type="button" disabled={loading || pending[`load:${m.path}`]} onclick={() => load(m.path)}>Reload<span class="visually-hidden"> {r.title}</span></button>
        <button type="button" disabled={pending.unload} onclick={() => act('unload', api.unload)}>Unload</button>
        <a class="button primary" href={href('chat')}>Go to Chat</a>
      {:else if r.state === 'on_disk' && m}
        <button type="button" class="primary" disabled={loading || pending[`load:${m.path}`]} onclick={() => load(m.path)}>Load<span class="visually-hidden"> {r.title}</span></button>
      {:else if r.state === 'downloading' && e}
        <button type="button" class="danger" disabled={pending[`cancel:${e.id}`]} onclick={() => act(`cancel:${e.id}`, () => api.cancelDownload(e.id))}>
          Cancel<span class="visually-hidden"> download of {r.title}</span>
        </button>
      {:else if (r.state === 'partial' || r.state === 'available' || r.state === 'incomplete') && e}
        <button type="button" class="primary" disabled={pending[`dl:${e.id}`]} onclick={() => download(e)}>
          {e.partial ? resumeLabel(e.received ?? 0, e.bytes) : 'Download'}<span class="visually-hidden"> {r.title}</span>
        </button>
        {#if e.partial}
          <button type="button" class="danger" disabled={pending[`part:${e.id}`]} onclick={() => deletePartial(e)} title="Delete the partially downloaded file and free its space">
            Delete partial<span class="visually-hidden"> download of {r.title}</span>
          </button>
        {/if}
      {/if}
      {#if r.deleteName && r.state !== 'loaded' && r.state !== 'downloading'}
        <button type="button" class="danger" disabled={pending[`del:${r.key}`]} onclick={() => deleteModel(r)}>
          {confirming === r.key ? 'Delete' : 'Delete...'}<span class="visually-hidden"> {r.title} from the disk</span>
        </button>
        {#if confirming === r.key}<button type="button" onclick={() => (confirming = '')}>Keep</button>{/if}
      {/if}
      {#if e?.added && r.state !== 'downloading'}
        <button type="button" disabled={pending[`rm:${e.id}`]} onclick={() => forget(e)} title="Take it off this list; files on disk stay">
          Forget<span class="visually-hidden"> {r.title}</span>
        </button>
      {/if}
    </div>
  </li>
{/snippet}

{#if data}
  {#if empty}
    <section class="card first">
      <h2>Get your first model</h2>
      <ol class="three">
        <li><strong>Find a model</strong>: search by name, or paste a repository.</li>
        <li><strong>Check it fits here</strong>: the server reads its header and measures it against this machine, before any download.</li>
        <li><strong>Download, then Load</strong>: Load appears on its row in Your models.</li>
      </ol>
      <HuggingFace onAdded={added} />
      {#if suggested.length}
        <h3>Or start from a suggestion</h3>
        <div class="suggestions">
          {#each suggested as r (r.key)}
            {#if r.entry}
              {@const e = r.entry}
              <button type="button" disabled={pending[`dl:${e.id}`]} onclick={() => download(e)} title={e.blurb}>
                Download {e.title}{e.quant ? ` (${e.quant})` : ''}, {formatBytes(e.bytes)}
              </button>
            {/if}
          {/each}
        </div>
        <!-- A suggestion whose download failed before a byte landed is still a suggestion: its
             failure is said here, beside the button that started it. -->
        {#each suggested as r (r.key)}
          {@const dl = r.entry ? server.downloads[r.entry.id] : undefined}
          {#if dl?.state === 'error'}
            <p class="err" role="alert">{r.title}: download failed: {dl.error}</p>
          {/if}
        {/each}
      {/if}
      <details class="more">
        <summary>Or paste a direct link to a gguf</summary>
        <UrlImport onAdded={added} />
      </details>
    </section>
  {:else}
    <h2>Your models</h2>
    <ul class="rows" aria-label="Your models">
      {#each yours as r (r.key)}{@render row(r)}{/each}
    </ul>
    {#if localGroups.companions.length}
      <details class="companions">
        <summary class="muted">{localGroups.companions.length} companion {localGroups.companions.length === 1 ? 'file' : 'files'}: not models to load, by their headers</summary>
        <ul>
          {#each localGroups.companions as m (m.path)}
            {@const name = m.dir === data.models_dir ? (m.folder ? `${m.folder}/${m.name}` : m.name) : ''}
            <li>
              <span class="name">{m.name}</span>
              <span class="badge">{roleLabel(m.role ?? '')}</span>
              <span class="muted">{formatBytes(m.bytes)}{m.arch ? `, ${m.arch}` : ''}{m.folder ? `, in ${m.folder}` : ''}</span>
              {#if name}
                <button type="button" class="danger small" disabled={pending[`del:${m.path}`]} onclick={() => deleteModel({ key: m.path, title: m.name, local: m, entry: null, state: 'on_disk', suggested: false, deleteName: name })}>
                  {confirming === m.path ? 'Delete' : 'Delete...'}<span class="visually-hidden"> {m.name}</span>
                </button>
                {#if confirming === m.path}<button type="button" class="small" onclick={() => (confirming = '')}>Keep</button>{/if}
              {/if}
            </li>
          {/each}
        </ul>
      </details>
    {/if}
    {#if suggested.length}
      <h3 class="suggested-title">Suggested</h3>
      <ul class="rows" aria-label="Suggested models">
        {#each suggested as r (r.key)}{@render row(r)}{/each}
      </ul>
    {/if}

    <h2>Get another model</h2>
    <HuggingFace onAdded={added} />
    <details class="card more">
      <summary>Any other model: a direct link to a gguf</summary>
      <UrlImport onAdded={added} />
    </details>
  {/if}

  <details class="where">
    <summary class="muted">Where models live</summary>
    <p class="muted">
      Downloads land in <code>{data.models_dir}</code>; a gguf you copy there (or into a folder directly inside it) is listed too.
      {#if data.scan_dirs?.length}Also listed, and loaded in place: {#each data.scan_dirs as d, i (d)}{i ? ', ' : ''}<code>{d}</code>{/each}.{/if}
    </p>
  </details>
{:else if !error}
  <div class="skel" aria-hidden="true">{#each [0, 1, 2] as i (i)}<span class="skeleton"></span>{/each}</div>
{/if}

</div>

<style>
  .name {
    overflow-wrap: anywhere;
  }
  .rows {
    list-style: none;
    padding: 0;
    margin: 0;
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }
  .row {
    display: flex;
    flex-wrap: wrap;
    gap: var(--s2) var(--s4);
    align-items: center;
    padding: var(--s3) var(--s4);
    border: var(--edge) solid var(--line);
    border-radius: var(--radius);
    background: var(--surface);
    box-shadow: var(--shadow-sm);
  }
  .skel {
    display: flex;
    flex-direction: column;
    gap: var(--s2);
  }
  .skel .skeleton {
    height: 4.5rem;
    border-radius: var(--radius);
  }
  .row.loaded {
    border-color: var(--ok);
  }
  .row.followed {
    border-color: var(--accent);
  }
  .what {
    flex: 1 1 20rem;
    min-width: 0;
  }
  .title {
    display: flex;
    align-items: center;
    gap: 0.4rem;
    flex-wrap: wrap;
  }
  .meta,
  .blurb,
  .note,
  .err {
    margin: 0.3rem 0 0;
    font-size: 0.85rem;
    overflow-wrap: anywhere;
  }
  .err {
    color: var(--err);
  }
  .actions {
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem;
    align-items: center;
  }
  .companions {
    margin-top: 0.5rem;
  }
  .companions summary,
  .where summary,
  .more summary {
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
  .companions li {
    display: flex;
    flex-wrap: wrap;
    gap: 0.4rem;
    align-items: center;
  }
  .small {
    font-size: 0.8rem;
    padding: 0.1rem 0.5rem;
  }
  .suggested-title {
    font-size: 0.95rem;
    margin: 1rem 0 0.5rem;
  }
  .first h2 {
    margin-top: 0;
  }
  .first h3 {
    font-size: 0.95rem;
    margin: 1rem 0 0.5rem;
  }
  .three {
    margin: 0 0 0.75rem;
    padding-left: 1.25rem;
  }
  .three li {
    margin: 0.2rem 0;
  }
  .suggestions {
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem;
  }
  .more {
    margin-top: 0.75rem;
  }
  .more :global(form) {
    margin-top: 0.5rem;
  }
  .where {
    margin-top: 1.5rem;
  }
</style>
