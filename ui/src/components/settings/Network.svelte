<script lang="ts">
  // Serve on the network (docs/screens.md, "Settings"): the app's server listens on every address
  // behind a key, so another computer or a phone uses this engine as an OpenAI-compatible server.
  // The same card as the phones'; the app restarts its server on every change.
  import { desktopState } from '../../stores/desktop.svelte';
  import { t } from '../../lib/text';

  let open = $state(false);
  let showKey = $state(false);
  let busy = $state(false);
  let copied = $state('');

  let on = $derived(desktopState.prefs.serveOnNetwork === true);
  let addresses = $derived(desktopState.info?.network?.addresses ?? []);
  let key = $derived(desktopState.info?.network?.key ?? '');
  let status = $derived(!on ? t('network.off') : addresses.length ? t('network.on', { address: addresses[0] }) : t('network.noNetwork'));

  async function toggle(next: boolean) {
    busy = true;
    try {
      await desktopState.setServeOnNetwork(next);
    } finally {
      busy = false;
    }
  }

  async function newKey() {
    busy = true;
    try {
      await desktopState.newNetworkKey();
    } finally {
      busy = false;
    }
  }

  async function copy(text: string) {
    try {
      await navigator.clipboard.writeText(text);
      copied = text;
      setTimeout(() => (copied = copied === text ? '' : copied), 2000);
    } catch {
      // The text stays selectable on the page.
    }
  }
</script>

<details class="card section" bind:open>
  <summary>
    <span class="title">{t('network.title')}</span>
    <span class="status" class:warn={on && addresses.length === 0} class:on={on && addresses.length > 0}>{status}</span>
  </summary>
  <div class="setting">
    <label class="what" for="serve-network">
      <span class="label">{t('network.title')}</span>
      <span class="muted small">{t('network.help')}</span>
    </label>
    <input id="serve-network" type="checkbox" class="switch" role="switch" checked={on} disabled={busy} onchange={(e) => toggle(e.currentTarget.checked)} />
  </div>
  {#if on}
    {#if addresses.length === 0}
      <p class="banner warn">{t('network.noNetwork.help')}</p>
    {:else}
      <h3>{t('network.address')}</h3>
      {#each addresses as a (a)}
        <div class="row">
          <code class="big">{a}</code>
          <button type="button" onclick={() => copy(a)}>{copied === a ? t('network.copied') : t('network.copy')}</button>
        </div>
      {/each}
    {/if}
    <h3>{t('network.key')}</h3>
    <div class="row">
      <code class="big">{showKey ? key : '•'.repeat(24)}</code>
      <button type="button" onclick={() => (showKey = !showKey)}>{showKey ? t('network.hide') : t('network.show')}</button>
      <button type="button" onclick={() => copy(key)} disabled={!key}>{copied === key && key ? t('network.copied') : t('network.copy')}</button>
      <button type="button" onclick={newKey} disabled={busy}>{t('network.newKey')}</button>
    </div>
    {#if addresses.length}<p class="muted small">{t('network.client', { url: `${addresses[0]}/v1` })}</p>{/if}
    <p class="warn-text small">{t('network.unencrypted')}</p>
  {/if}
</details>

<style>
  summary {
    cursor: pointer;
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: var(--s1) var(--s3);
  }
  .title {
    font-size: 1.0625rem;
    font-weight: 650;
  }
  .status {
    color: var(--muted);
    font-size: 0.875rem;
  }
  .status.on {
    color: var(--ok);
  }
  .status.warn,
  .warn-text {
    color: var(--warn);
  }
  h3 {
    margin: var(--s4) 0 var(--s2);
    font-size: 0.9375rem;
  }
  .row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: var(--s2);
    margin-bottom: var(--s2);
  }
  .big {
    flex: 1 1 16rem;
    min-width: 0;
    overflow-wrap: anywhere;
    font-size: 0.9375rem;
  }
  .small {
    font-size: 0.8125rem;
  }
</style>
