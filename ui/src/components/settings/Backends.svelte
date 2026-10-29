<script lang="ts">
  // Graphics backends (docs/desktop-app.md, "Backend packs"): what this server loaded, the packs the
  // release offers for this machine and which of them apply. Installing one downloads and checks
  // it; the server then has to restart, which the app does and a browser leaves to the person.
  import { onMount } from 'svelte';
  import Icon from '../Icon.svelte';
  import ProgressBar from '../ProgressBar.svelte';
  import { ApiError } from '../../lib/api';
  import type { Backends } from '../../lib/agent';
  import { agentApi } from '../../lib/agentApi';
  import { desktop, isDesktop } from '../../lib/desktop';
  import { formatBytes } from '../../lib/format';
  import { readProgress, type Progress } from '../../lib/progressEvent';
  import { server } from '../../stores/server.svelte';
  import { t } from '../../lib/text';

  let { compact = false }: { compact?: boolean } = $props();

  let data = $state<Backends | null>(null);
  let error = $state('');
  let missing = $state(false);
  let installing = $state('');
  let progress = $state<Progress | null>(null);
  let installError = $state('');
  let restart = $state(false);
  let restarting = $state(false);

  async function load() {
    try {
      data = await agentApi.backends();
      error = '';
    } catch (e) {
      if (e instanceof ApiError && e.status === 404) missing = true;
      else error = e instanceof Error ? e.message : String(e);
    }
  }
  onMount(() => {
    load();
    return server.subscribe((ev) => {
      if (ev.event !== 'backend' || !installing) return;
      const p = readProgress(ev.data);
      if (p && (!p.id || p.id === installing)) progress = p;
    });
  });

  async function install(id: string) {
    installing = id;
    progress = null;
    installError = '';
    try {
      await agentApi.installBackend(id);
      restart = true;
      await load();
    } catch (e) {
      installError = e instanceof Error ? e.message : String(e);
    } finally {
      installing = '';
    }
  }

  async function restartNow() {
    restarting = true;
    await desktop.restartServer();
  }

  let applicable = $derived((data?.packs ?? []).filter((p) => p.applies));
  let others = $derived((data?.packs ?? []).filter((p) => !p.applies));
</script>

<section class:card={!compact} class="section" aria-labelledby="backends-title">
  {#if !compact}<h2 id="backends-title">{t('backends.title')}</h2>{/if}
  {#if missing}
    <p class="muted">{t('backends.unavailable')}</p>
  {:else if error}
    <p class="banner error" role="alert">{t('backends.failed')} {error} <button type="button" class="small" onclick={load}>{t('action.retry')}</button></p>
  {:else if !data}
    <span class="skeleton line"></span><span class="skeleton line short"></span>
  {:else}
    {#if !compact}
      <div class="setting">
        <div class="what">
          <span class="label">{t('backends.loaded')}</span>
          <span class="muted small">{t('backends.loaded.help')}</span>
        </div>
        <div class="badges">
          {#each data.registered as b (b)}<span class="badge ok">{b}</span>{:else}<span class="muted small">{t('backends.cpuOnly')}</span>{/each}
        </div>
      </div>
    {/if}
    {#if data.feed_error}<p class="muted small">{t('backends.feedFailed')} {data.feed_error}</p>{/if}
    {#if applicable.length === 0 && !compact}
      <p class="muted small">{t('backends.noneApply')}</p>
    {/if}
    {#each applicable as p (p.id)}
      <div class="setting">
        <div class="what">
          <span class="label">{p.title}</span>
          <span class="muted small">{p.installed ? t('backends.installed') : t('backends.applies')}{p.size ? ` · ${formatBytes(p.size)}` : ''}</span>
          {#if installing === p.id}
            {#if progress && progress.total > 0}
              <ProgressBar received={progress.received} total={progress.total} label={t('backends.installing', { name: p.title })} />
            {:else}
              <span class="muted small" role="status">{t('backends.installing', { name: p.title })}</span>
            {/if}
          {/if}
        </div>
        {#if p.installed}
          <span class="badge ok"><Icon name="check" size={13} />{t('backends.installed.short')}</span>
        {:else}
          <button type="button" class="primary" disabled={!!installing} onclick={() => install(p.id)}><Icon name="download" size={16} />{t('backends.install')}</button>
        {/if}
      </div>
    {/each}
    {#if others.length && !compact}
      <details class="others">
        <summary class="muted small">{t('backends.others', { n: others.length })}</summary>
        <ul>
          {#each others as p (p.id)}<li class="small">{p.title}</li>{/each}
        </ul>
      </details>
    {/if}
    {#if installError}<p class="banner error" role="alert">{t('backends.installFailed')} {installError}</p>{/if}
    {#if restart}
      <div class="banner info" role="status">
        <span>{isDesktop() ? t('backends.restart.app') : t('backends.restart.browser')}</span>
        {#if isDesktop()}<button type="button" class="primary small" disabled={restarting} onclick={restartNow}>{t('backends.restart')}</button>{/if}
      </div>
    {/if}
  {/if}
</section>

<style>
  .badges {
    display: flex;
    gap: var(--s1);
    flex-wrap: wrap;
  }
  .line {
    height: 1rem;
    margin: var(--s2) 0;
  }
  .short {
    width: 50%;
  }
  .others ul {
    margin: var(--s1) 0 0;
    padding-left: var(--s5);
  }
  .banner {
    margin-top: var(--s3);
  }
</style>
