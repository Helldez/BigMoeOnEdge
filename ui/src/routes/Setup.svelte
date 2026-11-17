<script lang="ts">
  // The first run (docs/screens.md, "First run"): one path to the first private answer, in real
  // steps. Where a graphics pack applies to this computer and is missing, the first step installs
  // it (or the person skips it, and the processor answers); then pick a model (one that fits in
  // memory beside one larger than it), its download with the time left, and the model loads by
  // itself when the download ends; the conversation opens with a few questions to try. Skip and
  // "Already have gguf files?" stay one click away throughout.
  import { onMount, untrack } from 'svelte';
  import Icon from '../components/Icon.svelte';
  import ProgressBar from '../components/ProgressBar.svelte';
  import { api } from '../lib/api';
  import type { Backends } from '../lib/agent';
  import { agentApi } from '../lib/agentApi';
  import { desktop, isDesktop } from '../lib/desktop';
  import { graphicsState } from '../lib/graphics';
  import { readProgress, type Progress } from '../lib/progressEvent';
  import { addSample, humanDuration, rate, secondsLeft, type Sample } from '../lib/eta';
  import { formatBytes, resumeLabel } from '../lib/format';
  import { libraryRows, recommendedEntry } from '../lib/library';
  import { href } from '../lib/route';
  import type { CatalogEntry, ModelsResponse } from '../lib/types';
  import { agent } from '../stores/agent.svelte';
  import { t } from '../lib/text';
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
  // The graphics: read with the release feed, since a first run is online anyway (it downloads a
  // model), so the pack a card needs is known before the model is picked.
  let backends = $state<Backends | null>(null);
  async function readBackends(fromNetwork: boolean) {
    try {
      backends = await agentApi.backends(fromNetwork);
    } catch {
      backends = null; // a server without packs: the step never shows
    }
  }
  onMount(() => {
    refresh();
    readBackends(true);
    return server.subscribe((ev) => {
      if (ev.event !== 'backend' || !installing) return;
      const p = readProgress(ev.data);
      if (p && (!p.id || p.id === installing)) packProgress = p;
    });
  });
  let gfx = $derived(backends ? graphicsState(backends) : null);
  // The graphics step is on the path when a pack was offered at the start of this visit; it is done
  // once the pack is installed or the person chose the processor.
  let offered = $state(false);
  $effect(() => {
    if (gfx?.kind === 'offer') offered = true;
  });
  let skipped = $state(false);
  let installing = $state('');
  let packProgress = $state<Progress | null>(null);
  let packError = $state('');
  let packDone = $state(false);
  let graphicsPending = $derived(offered && !skipped && !packDone && gfx?.kind === 'offer');
  async function installPack(id: string) {
    installing = id;
    packProgress = null;
    packError = '';
    try {
      await agentApi.installBackend(id);
      packDone = true;
      // The pack loads at the engine's next start: the app restarts it; a browser cannot.
      if (isDesktop()) await desktop.restartServer();
      await readBackends(false);
    } catch (e) {
      packError = e instanceof Error ? e.message : String(e);
    } finally {
      installing = '';
    }
  }
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
  let gpus = $derived((plan?.devices ?? []).filter((d) => !d.cpu).map((d) => d.description || d.name));
  let suggested = $derived(data ? recommendedEntry(data.catalog, ramBytes) : null);
  // Without a pick of this visit: a download that runs, then one that stopped part way, then the
  // suggestion; so the screen reopens where the person left it.
  let inFlight = $derived(
    data?.catalog.find((e) => server.downloads[e.id]?.state === 'running') ?? data?.catalog.find((e) => e.partial) ?? null,
  );
  let chosen = $derived<CatalogEntry | null>(data?.catalog.find((e) => e.id === pick) ?? inFlight ?? suggested);
  let rows = $derived(data ? libraryRows(data, data.local, server.downloads, server.info?.model?.path ?? null) : []);
  let row = $derived(chosen ? rows.find((r) => r.entry?.id === chosen.id) : undefined);
  let dl = $derived(chosen ? server.downloads[chosen.id] : undefined);
  // Side by side, the two first runs this product offers: the largest model that fits in memory
  // (the fastest answers) and the smallest that does not (what streaming from disk is for). No
  // speed is promised for either: nothing is measured before a model is on disk.
  let pair = $derived.by(() => {
    const offered = (data?.catalog ?? []).filter((e) => !e.added && e.bytes > 0);
    const fits = offered.filter((e) => e.bytes <= ramBytes).sort((a, b) => b.bytes - a.bytes)[0];
    const big = offered.filter((e) => e.bytes > ramBytes).sort((a, b) => a.bytes - b.bytes)[0];
    return fits && big && ramBytes > 0 ? [fits, big] : [];
  });

  let loadingNow = $derived(server.info?.state === 'loading' || server.info?.state === 'planning');
  // Where the person is on the path: the step follows the model's state, never a button.
  // A paused download is still the download step (with Resume), unless the person asked to pick again.
  let repick = $state(false);
  let paused = $derived(!!chosen?.partial && row?.state !== 'downloading');
  let modelStep = $derived<1 | 2 | 3>(
    loadingNow || row?.state === 'on_disk' || row?.state === 'loaded'
      ? 3
      : row?.state === 'downloading' || (paused && !repick)
        ? 2
        : 1,
  );
  // The steps shown: the graphics one first when it is on the path.
  let STEPS = $derived(offered ? (['setup.step.graphics', 'setup.step.pick', 'setup.step.download', 'setup.step.start'] as const) : (['setup.step.pick', 'setup.step.download', 'setup.step.start'] as const));
  let current = $derived(graphicsPending ? 1 : modelStep + (offered ? 1 : 0));
  // The model's own step, for the panels below (1 pick, 2 download, 3 start).
  let step = $derived(modelStep);

  // The download's pace, from the progress the server reports, for the time left.
  let samples = $state<Sample[]>([]);
  $effect(() => {
    const got = dl?.received ?? 0;
    const downloading = step === 2;
    // The samples are written here, never a reason to run again: read them untracked.
    untrack(() => {
      if (downloading && got > 0) samples = addSample(samples, { at: performance.now(), received: got });
      else if (!downloading && samples.length) samples = [];
    });
  });
  let total = $derived(dl?.total || chosen?.bytes || 0);
  let left = $derived(secondsLeft(samples, total));
  let speed = $derived(rate(samples));

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

  // Step 3 starts by itself: a downloaded model is loaded and the conversation opens, so the first
  // answer is one question away. Once per visit, and never over a load already running.
  let started = $state(false);
  async function start() {
    const path = row?.local?.path;
    if (!path || started) return;
    started = true;
    busy = true;
    actionError = '';
    try {
      await api.load(path);
    } catch (e) {
      started = false;
      actionError = e instanceof Error ? e.message : String(e);
    } finally {
      busy = false;
    }
  }
  $effect(() => {
    if (step === 3 && !loadingNow && row?.state === 'on_disk' && !actionError) start();
  });
  // The conversation opens when the model can answer, not while it still loads.
  $effect(() => {
    if (started && server.ready) {
      agent.newConversation();
      location.hash = href('chat');
    }
  });

  const gb = (mib: number) => Math.round(mib / 1024);
  // What the graphics are, said under the machine's facts when there is no step to take.
  let gfxLine = $derived(
    !gfx
      ? ''
      : gfx.kind === 'ready'
        ? gfx.cards.length
          ? t('setup.graphics.using', { names: gfx.cards.join(', ') })
          : t('setup.graphics.cpu')
        : gfx.kind === 'installed'
          ? packDone && !isDesktop()
            ? t('setup.graphics.restart', { name: gfx.pack.title })
            : t('setup.graphics.installed', { name: gfx.pack.title })
          : gfx.kind === 'unchecked'
            ? t('setup.graphics.unknown')
            : '',
  );
</script>

<div class="page-col setup">
  <header class="hero">
    <img src="/logo.svg" alt="" width="64" height="64" />
    <h1>{t('setup.title')}</h1>
    <p class="muted">{t('setup.lead')}</p>
    {#if host}
      <p class="machine small">
        {[t('setup.memory', { gb: gb(host.ram_total_mib) }), t('setup.cores', { n: host.cpu_threads }), ...gpus].join(' · ')}
      </p>
    {/if}
    {#if gfxLine && !graphicsPending}<p class="machine small">{gfxLine}</p>{/if}
  </header>

  <ol class="progress-steps" aria-label={t('setup.steps')}>
    {#each STEPS as key, i (key)}
      <li class:done={current > i + 1} class:now={current === i + 1} aria-current={current === i + 1 ? 'step' : undefined}>
        <span class="n" aria-hidden="true">{#if current > i + 1}<Icon name="check" size={14} />{:else}{i + 1}{/if}</span>
        <span>{t(key)}</span>
      </li>
    {/each}
  </ol>

  <section class="sheet" aria-live="polite">
    {#if error}
      <p class="banner error" role="alert">{t('setup.modelsFailed')} {error} <button type="button" class="small" onclick={refresh}>{t('action.retry')}</button></p>
    {:else if !data}
      <span class="skeleton line"></span><span class="skeleton line"></span>
    {:else if graphicsPending && gfx?.kind === 'offer'}
      <!-- The graphics card: what it is, what it needs, what happens next; or the processor. -->
      <h2>{t('setup.graphics.title')}</h2>
      <p>{t('setup.graphics.lead', { name: gfx.pack.title })}</p>
      <p class="muted small">{isDesktop() ? t('setup.graphics.after.app') : t('setup.graphics.after.browser')}</p>
      {#if installing}
        {#if packProgress && packProgress.total > 0}
          <ProgressBar received={packProgress.received} total={packProgress.total} label={t('backends.installing', { name: gfx.pack.title })} />
        {:else}
          <p class="muted" role="status">{t('backends.installing', { name: gfx.pack.title })}</p>
        {/if}
      {:else}
        <div class="actions">
          <button type="button" class="primary" onclick={() => installPack(gfx.pack.id)}>
            <Icon name="download" size={16} />{gfx.pack.size ? t('setup.graphics.install.size', { name: gfx.pack.title, size: formatBytes(gfx.pack.size) }) : t('setup.graphics.install', { name: gfx.pack.title })}
          </button>
          <button type="button" onclick={() => (skipped = true)}>{t('setup.graphics.skip')}</button>
        </div>
      {/if}
      {#if packError}<p class="banner error" role="alert">{t('backends.installFailed')} {packError}</p>{/if}
    {:else if !chosen}
      <p>{t('setup.noCatalog')} <a href={href('models')}>{t('nav.models')}</a></p>
    {:else if step === 1}
      <h2>{t('setup.pick.title')}</h2>
      {#if pair.length === 2}
        <div class="choices" role="radiogroup" aria-label={t('setup.pick.title')}>
          {#each pair as e (e.id)}
            <button type="button" role="radio" aria-checked={chosen.id === e.id} class="choice" onclick={() => (pick = e.id)}>
              <span class="kind">{e.bytes <= ramBytes ? t('setup.kind.fits') : t('setup.kind.streams')}</span>
              <strong>{e.title}</strong>
              <span class="muted small">{formatBytes(e.bytes)}</span>
              <span class="small">{e.bytes <= ramBytes ? t('setup.fits') : t('setup.streams')}</span>
            </button>
          {/each}
        </div>
      {:else}
        <div class="single">
          <strong>{chosen.title}</strong>
          <span class="muted small">{formatBytes(chosen.bytes)}</span>
          <span class="small">{chosen.bytes <= ramBytes ? t('setup.fits') : t('setup.streams')}</span>
        </div>
      {/if}
      <div class="actions">
        <button type="button" class="primary" disabled={busy} onclick={() => { repick = false; download(); }}>
          <Icon name="download" size={16} />{chosen.partial ? resumeLabel(chosen.received ?? 0, chosen.bytes) : t('setup.download.size', { size: formatBytes(chosen.bytes) })}
        </button>
        <span class="muted small">{t('setup.download.help')}</span>
      </div>
      {#if dl?.state === 'error'}<p class="banner error" role="alert">{t('setup.downloadFailed')} {dl.error}</p>{/if}
    {:else if step === 2 && paused}
      <h2>{t('setup.paused', { name: chosen.title })}</h2>
      <ProgressBar received={chosen.received ?? 0} {total} label={t('setup.paused', { name: chosen.title })} />
      <p class="muted small">{t('setup.paused.help')}</p>
      {#if dl?.state === 'error'}<p class="banner error" role="alert">{t('setup.downloadFailed')} {dl.error}</p>{/if}
      <div class="actions">
        <button type="button" class="primary" disabled={busy} onclick={download}>
          <Icon name="download" size={16} />{resumeLabel(chosen.received ?? 0, chosen.bytes)}
        </button>
        <button type="button" onclick={() => (repick = true)}>{t('setup.repick')}</button>
      </div>
    {:else if step === 2}
      <h2>{t('setup.downloading', { name: chosen.title })}</h2>
      <ProgressBar received={dl?.received ?? chosen.received ?? 0} {total} label={t('setup.downloading', { name: chosen.title })} />
      <p class="eta">
        {#if left !== null}
          <strong>{t('setup.left', { time: humanDuration(left) })}</strong>
          {#if speed}<span class="muted"> · {t('setup.speed', { rate: formatBytes(speed) })}</span>{/if}
        {:else}
          <span class="muted">{t('setup.measuring')}</span>
        {/if}
      </p>
      <p class="muted small">{t('setup.download.keep')}</p>
      {#if dl?.error}<p class="muted small">{dl.error}</p>{/if}
      <div class="actions">
        <button type="button" onclick={() => api.cancelDownload(chosen.id).then(refresh)}>{t('action.pause')}</button>
      </div>
    {:else}
      <h2>{t('setup.starting', { name: chosen.title })}</h2>
      <p class="muted">{t('setup.starting.help')}</p>
      {#if actionError}
        <p class="banner error" role="alert">{actionError}</p>
        <div class="actions"><button type="button" class="primary" onclick={() => { actionError = ''; start(); }}>{t('action.retry')}</button></div>
      {:else}
        <span class="skeleton line"></span>
      {/if}
    {/if}
    {#if actionError && step !== 3}<p class="banner error" role="alert">{actionError}</p>{/if}
  </section>

  <p class="aside small">
    <a href="{href('models')}/add-folder">{t('setup.have')}</a>
    <span aria-hidden="true">·</span>
    <a href={href('models')}>{t('setup.more')}</a>
    <span aria-hidden="true">·</span>
    <a href="#/chat">{t('setup.skip')}</a>
  </p>
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
  /* The chip on a sheet over the seal's yellow rule, as on the conversation's welcome. */
  .hero img {
    image-rendering: pixelated;
    padding: 12px;
    border-radius: var(--radius);
    background: var(--surface);
    box-shadow: var(--shadow-md);
    border-bottom: 3px solid var(--seal);
    box-sizing: content-box;
  }
  .hero h1 {
    margin: var(--s3) 0 0;
  }
  .hero p {
    margin: 0;
    max-width: 32rem;
  }
  .machine {
    color: var(--muted);
  }
  /* Where the person is: three numbered steps, the current one in ink, the done ones checked. */
  .progress-steps {
    list-style: none;
    margin: 0 0 var(--s4);
    padding: 0;
    display: flex;
    justify-content: center;
    gap: var(--s5);
    flex-wrap: wrap;
    color: var(--muted);
    font-size: 0.875rem;
    font-weight: 550;
  }
  .progress-steps li {
    display: flex;
    align-items: center;
    gap: var(--s2);
  }
  .n {
    width: 1.5rem;
    height: 1.5rem;
    border-radius: 50%;
    display: grid;
    place-items: center;
    border: 1.5px solid var(--line-strong);
    font-size: 0.75rem;
  }
  .now {
    color: var(--text);
  }
  .now .n {
    background: var(--accent-fill);
    border-color: var(--accent-fill);
    color: var(--on-accent);
  }
  .done .n {
    border-color: var(--ok);
    color: var(--ok);
  }
  .sheet {
    background: var(--surface);
    border-radius: var(--radius-sm);
    box-shadow: var(--shadow-md);
    padding: var(--s5);
    display: flex;
    flex-direction: column;
    gap: var(--s3);
  }
  .sheet h2 {
    margin: 0;
    font-size: 1.125rem;
  }
  .choices {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(14rem, 1fr));
    gap: var(--s3);
  }
  .choice {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    justify-content: flex-start;
    gap: var(--s1);
    padding: var(--s3);
    text-align: left;
    white-space: normal;
    font-weight: 400;
    line-height: 1.4;
    box-shadow: none;
  }
  .choice[aria-checked='true'] {
    border-color: var(--accent);
    box-shadow: inset 0 0 0 1px var(--accent);
    background: var(--accent-tint);
  }
  .choice strong {
    overflow-wrap: anywhere;
  }
  .kind {
    font-size: 0.8125rem;
    font-weight: 650;
    color: var(--accent-ink);
  }
  .single {
    display: flex;
    flex-direction: column;
    gap: var(--s1);
  }
  .actions {
    display: flex;
    align-items: center;
    gap: var(--s3);
    flex-wrap: wrap;
  }
  .eta {
    margin: 0;
  }
  .line {
    height: 1rem;
  }
  .aside {
    display: flex;
    justify-content: center;
    gap: var(--s2);
    flex-wrap: wrap;
    margin-top: var(--s4);
    color: var(--muted);
  }
  .sheet :global(.section) {
    margin: 0;
  }
</style>
