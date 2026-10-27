<script lang="ts">
  // The desktop app's own preferences (docs/desktop-app.md), shown only inside the app.
  import { desktopState } from '../../stores/desktop.svelte';
  import { t } from '../../stores/prefs.svelte';
  import Segmented from './Segmented.svelte';

  const SCALES = [0.9, 1, 1.1, 1.25, 1.5];
  let port = $state(String(desktopState.prefs.port ?? ''));
  let portError = $state('');
  let portSaved = $state(false);

  async function savePort() {
    const n = Number(port);
    if (!Number.isInteger(n) || n < 1024 || n > 65535) {
      portError = t('app.port.invalid');
      return;
    }
    portError = '';
    await desktopState.setPrefs({ port: n });
    portSaved = true;
  }
</script>

<section class="card section" aria-labelledby="app-title">
  <h2 id="app-title">{t('settings.app')}</h2>
  {#if desktopState.info}<p class="muted small">{t('app.version', { version: desktopState.info.version, target: desktopState.info.target })}</p>{/if}
  <div class="setting">
    <label class="what" for="tray">
      <span class="label">{t('app.tray')}</span>
      <span class="muted small">{t('app.tray.help')}</span>
    </label>
    <input id="tray" type="checkbox" class="switch" role="switch" checked={desktopState.prefs.closeToTray === true} onchange={(e) => desktopState.setPrefs({ closeToTray: e.currentTarget.checked })} />
  </div>
  <div class="setting">
    <label class="what" for="notify">
      <span class="label">{t('app.notifications')}</span>
      <span class="muted small">{t('app.notifications.help')}</span>
    </label>
    <input id="notify" type="checkbox" class="switch" role="switch" checked={desktopState.prefs.notifications !== false} onchange={(e) => desktopState.setPrefs({ notifications: e.currentTarget.checked })} />
  </div>
  <div class="setting">
    <div class="what">
      <span class="label">{t('app.scale')}</span>
      <span class="muted small">{t('app.scale.help')}</span>
    </div>
    <Segmented label={t('app.scale')} value={desktopState.prefs.uiScale ?? 1} options={SCALES.map((v) => ({ value: v, label: `${Math.round(v * 100)}%` }))} onchange={(v: number) => desktopState.setPrefs({ uiScale: v })} />
  </div>
  <div class="setting">
    <label class="what" for="port">
      <span class="label">{t('app.port')}</span>
      <span class="muted small">{t('app.port.help')}</span>
    </label>
    <form
      class="row"
      onsubmit={(e) => {
        e.preventDefault();
        savePort();
      }}
    >
      <input id="port" type="number" min="1024" max="65535" class="port figure" bind:value={port} />
      <button type="submit">{t('action.apply')}</button>
    </form>
  </div>
  {#if portError}<p class="err small" role="alert">{portError}</p>{/if}
  {#if portSaved}<p class="muted small" role="status">{t('app.port.saved')}</p>{/if}
</section>

<style>
  .port {
    width: 7rem;
  }
  .err {
    color: var(--err);
  }
</style>
