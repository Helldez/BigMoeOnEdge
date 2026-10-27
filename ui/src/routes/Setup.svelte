<script lang="ts">
  // The first run (docs/screens.md, "First run"): with no model on this computer, what it has in
  // plain words, the graphics pack to install where one applies, the catalog's model that suits its
  // memory, its download (resumable), then straight into a conversation.
  import { onMount } from 'svelte';
  import Icon from '../components/Icon.svelte';
  import ProgressBar from '../components/ProgressBar.svelte';
  import Backends from '../components/settings/Backends.svelte';
  import { api } from '../lib/api';
  import { formatBytes, resumeLabel } from '../lib/format';
  import { libraryRows, recommendedEntry } from '../lib/library';
  import { href } from '../lib/route';
  import type { CatalogEntry, ModelsResponse } from '../lib/types';
  import { agent } from '../stores/agent.svelte';
  import { t } from '../stores/prefs.svelte';
  import { server } from '../stores/server.svelte';

  let data = $state<ModelsResponse | null>(null);
  let error = $state('');
  let pick = $state('');
  let busy = $state(false);
  let actionError = $state('');

  async function refresh() {
    try {
      data = await api.models();
      error = '';
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
    }
  }
  onMount(refresh);
  let seenFinished = server.downloadsFinished;
  $effect(() => {
    if (server.downloadsFinished !== seenFinished) {
      seenFinished = server.downloadsFinished;
      refresh();
    }
  });

  let host = $derived(server.info?.host ?? null);
  let ramBytes = $derived((host?.ram_total_mib ?? 0) * 1024 * 1024);
  let plan = $derived(server.plan && server.plan.available ? server.plan : null);
  let devices = $derived((plan?.devices ?? []).filter((d) => !d.cpu));
  let suggested = $derived(data ? recommendedEntry(data.catalog, ramBytes) : null);
  let chosen = $derived<CatalogEntry | null>(data?.catalog.find((e) => e.id === pick) ?? suggested);
  let rows = $derived(data ? libraryRows(data, data.local, server.downloads, server.info?.model?.path ?? null) : []);
  let row = $derived(chosen ? rows.find((r) => r.entry?.id === chosen.id) : undefined);
  let dl = $derived(chosen ? server.downloads[chosen.id] : undefined);
  let others = $derived((data?.catalog ?? []).filter((e) => !e.added && e.id !== chosen?.id));

  async function download() {
    if (!chosen) return;
    busy = true;
    actionError = '';
    try {
      await api.download(chosen.id);
      await refresh();
    } catch (e) {
      actionError = e instanceof Error ? e.message : String(e);
    } finally {
      busy = false;
    }
  }

  async function start() {
    const path = row?.local?.path;
    if (!path) return;
    busy = true;
    actionError = '';
    try {
      await api.load(path);
      agent.newConversation();
      location.hash = href('chat');
    } catch (e) {
      actionError = e instanceof Error ? e.message : String(e);
    } finally {
      busy = false;
    }
  }

  const gb = (mib: number) => Math.round(mib / 1024);
</script>

<div class="page-col setup">
  <div class="hero">
    <img src="/logo.svg" alt="" width="48" height="48" />
    <h1>{t('setup.title')}</h1>
    <p class="muted">{t('setup.lead')}</p>
  </div>

  <ol class="steps">
    <li class="card step">
      <span class="num" aria-hidden="true">1</span>
      <div class="body">
        <h2>{t('setup.machine')}</h2>
        {#if host}
          <ul class="facts">
            <li><Icon name="chip" size={16} />{t('setup.memory', { gb: gb(host.ram_total_mib) })}</li>
            <li><Icon name="activity" size={16} />{t('setup.cores', { n: host.cpu_threads })}</li>
            {#each devices as d (d.name)}<li><Icon name="gauge" size={16} />{t('setup.device', { name: d.description || d.name })}</li>{/each}
          </ul>
          <p class="muted small">{t('setup.machine.help')}</p>
        {:else}
          <span class="skeleton line"></span>
        {/if}
      </div>
    </li>

    <li class="card step">
      <span class="num" aria-hidden="true">2</span>
      <div class="body">
        <h2>{t('setup.graphics')}</h2>
        <p class="muted small">{t('setup.graphics.help')}</p>
        <Backends compact />
      </div>
    </li>

    <li class="card step">
      <span class="num" aria-hidden="true">3</span>
      <div class="body">
        <h2>{t('setup.model')}</h2>
        {#if error}
          <p class="banner error" role="alert">{t('setup.modelsFailed')} {error} <button type="button" class="small" onclick={refresh}>{t('action.retry')}</button></p>
        {:else if !data}
          <span class="skeleton line"></span>
        {:else if !chosen}
          <p>{t('setup.noCatalog')} <a href={href('models')}>{t('nav.models')}</a></p>
        {:else}
          <div class="pick">
            <div class="what">
              <div class="row">
                <strong>{chosen.title}</strong>
                {#if chosen.quant}<span class="badge">{chosen.quant}</span>{/if}
                {#if chosen.id === suggested?.id}<span class="badge info">{t('setup.recommended')}</span>{/if}
              </div>
              <p class="muted small">{formatBytes(chosen.bytes)} · {chosen.blurb}</p>
              <p class="small">{chosen.bytes <= ramBytes ? t('setup.fits') : t('setup.streams')}</p>
            </div>
            {#if row?.state === 'downloading'}
              <div class="progress">
                <ProgressBar received={dl?.received ?? chosen.received ?? 0} total={dl?.total || chosen.bytes} label={t('setup.downloading', { name: chosen.title })} />
                {#if dl?.error}<p class="muted small">{dl.error}</p>{/if}
                <button type="button" class="small" onclick={() => api.cancelDownload(chosen.id).then(refresh)}>{t('action.pause')}</button>
              </div>
            {:else if row?.state === 'on_disk' || row?.state === 'loaded'}
              <button type="button" class="primary" disabled={busy || server.info?.state === 'loading' || server.info?.state === 'planning'} onclick={start}>
                <Icon name="play" size={16} />{t('setup.start')}
              </button>
            {:else}
              <button type="button" class="primary" disabled={busy} onclick={download}>
                <Icon name="download" size={16} />{chosen.partial ? resumeLabel(chosen.received ?? 0, chosen.bytes) : t('setup.download')}
              </button>
            {/if}
          </div>
          {#if dl?.state === 'error'}<p class="banner error" role="alert">{t('setup.downloadFailed')} {dl.error}</p>{/if}
          {#if others.length}
            <div class="others">
              <span class="muted small">{t('setup.others')}</span>
              {#each others as o (o.id)}
                <button type="button" class="small" onclick={() => (pick = o.id)}>{o.title} · {formatBytes(o.bytes)}</button>
              {/each}
            </div>
          {/if}
          <p class="small"><a href={href('models')}>{t('setup.more')}</a></p>
        {/if}
        {#if actionError}<p class="banner error" role="alert">{actionError}</p>{/if}
        {#if server.info?.state === 'loading' || server.info?.state === 'planning'}
          <p class="muted small" role="status">{t('status.loading.long')}</p>
        {/if}
      </div>
    </li>
  </ol>
  <p class="skip-setup small"><a href="#/chat">{t('setup.skip')}</a></p>
</div>

<style>
  .setup {
    max-width: 44rem;
  }
  .hero {
    text-align: center;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: var(--s2);
    margin: var(--s5) 0 var(--s5);
  }
  .hero img {
    image-rendering: pixelated;
  }
  .hero h1 {
    margin: var(--s2) 0 0;
  }
  .hero p {
    margin: 0;
    max-width: 32rem;
  }
  .steps {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: var(--s3);
  }
  .step {
    display: flex;
    gap: var(--s4);
  }
  .num {
    flex: none;
    width: 2rem;
    height: 2rem;
    border-radius: 50%;
    display: grid;
    place-items: center;
    background: var(--accent-tint);
    color: var(--accent-ink);
    font-weight: 650;
  }
  .body {
    flex: 1;
    min-width: 0;
  }
  .body h2 {
    margin: var(--s1) 0 var(--s2);
    font-size: 1.0625rem;
  }
  .facts {
    list-style: none;
    padding: 0;
    margin: 0 0 var(--s2);
    display: flex;
    flex-direction: column;
    gap: var(--s1);
  }
  .facts li {
    display: flex;
    align-items: center;
    gap: var(--s2);
  }
  .facts :global(svg) {
    color: var(--muted);
  }
  .pick {
    display: flex;
    gap: var(--s3);
    align-items: center;
    flex-wrap: wrap;
    padding: var(--s3);
    border: var(--edge) solid var(--line);
    border-radius: var(--radius);
    background: var(--bg);
  }
  .pick .what {
    flex: 1 1 16rem;
    min-width: 0;
  }
  .pick p {
    margin: var(--s1) 0 0;
  }
  .progress {
    flex: 1 1 14rem;
    display: flex;
    flex-direction: column;
    gap: var(--s1);
    align-items: flex-start;
  }
  .progress :global(.wrap) {
    width: 100%;
  }
  .others {
    display: flex;
    gap: var(--s2);
    align-items: center;
    flex-wrap: wrap;
    margin-top: var(--s3);
  }
  .line {
    height: 1rem;
  }
  .skip-setup {
    text-align: center;
    margin-top: var(--s4);
  }
  .body :global(.section) {
    margin: 0;
  }
</style>
