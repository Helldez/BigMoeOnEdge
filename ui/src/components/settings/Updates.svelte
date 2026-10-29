<script lang="ts">
  // Updates (docs/desktop-app.md, "Updates"): nothing checks in the background; this button reads
  // the release feed, and in the app a newer version is downloaded, checked, and installed.
  import Icon from '../Icon.svelte';
  import { ApiError } from '../../lib/api';
  import type { UpdateInfo } from '../../lib/agent';
  import { agentApi } from '../../lib/agentApi';
  import { desktop, isDesktop } from '../../lib/desktop';
  import { t } from '../../lib/text';

  let info = $state<UpdateInfo | null>(null);
  let checking = $state(false);
  let downloading = $state(false);
  let error = $state('');
  let ready = $state('');

  async function check() {
    checking = true;
    error = '';
    try {
      info = await agentApi.update();
    } catch (e) {
      error = e instanceof ApiError && e.status === 404 ? t('updates.unavailable') : e instanceof Error ? e.message : String(e);
    } finally {
      checking = false;
    }
  }

  async function download() {
    downloading = true;
    error = '';
    try {
      const r = await agentApi.downloadUpdate();
      ready = r.path;
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
    } finally {
      downloading = false;
    }
  }
</script>

<section class="card section" aria-labelledby="updates-title">
  <h2 id="updates-title">{t('updates.title')}</h2>
  <div class="setting">
    <div class="what">
      <span class="label">{info ? t('updates.current', { version: info.current }) : t('updates.check.help')}</span>
      {#if info}
        <span class="muted small" role="status">
          {info.available ? t('updates.available', { version: info.latest }) : t('updates.latest')}
        </span>
      {/if}
    </div>
    <button type="button" disabled={checking} onclick={check}><Icon name="refresh" size={16} />{checking ? t('updates.checking') : t('updates.check')}</button>
  </div>
  {#if info?.available}
    <div class="setting">
      <div class="what">
        <span class="label">{t('updates.new', { version: info.latest })}</span>
        {#if info.notes}<a class="small" href={info.notes} target="_blank" rel="noopener noreferrer">{t('updates.notes')}</a>{/if}
      </div>
      {#if isDesktop()}
        {#if ready}
          <button type="button" class="primary" onclick={() => desktop.applyUpdate(ready)}>{t('updates.install')}</button>
        {:else}
          <button type="button" class="primary" disabled={downloading} onclick={download}><Icon name="download" size={16} />{downloading ? t('updates.downloading') : t('updates.download')}</button>
        {/if}
      {:else}
        <span class="muted small">{t('updates.browser')}</span>
      {/if}
    </div>
    {#if ready}<p class="muted small">{t('updates.ready')}</p>{/if}
  {/if}
  {#if error}<p class="banner error" role="alert">{t('updates.failed')} {error}</p>{/if}
</section>
