<script lang="ts">
  // Settings (docs/screens.md, "Settings"): the UI's own (appearance, the desktop app), the saved
  // providers, graphics backends and updates, then every engine tunable from the parameter table.
  // A link to one parameter (`#/settings/<key>`) opens the engine section on it.
  import AppSettings from '../components/settings/AppSettings.svelte';
  import Appearance from '../components/settings/Appearance.svelte';
  import Backends from '../components/settings/Backends.svelte';
  import EngineSettings from '../components/settings/EngineSettings.svelte';
  import Providers from '../components/settings/Providers.svelte';
  import Updates from '../components/settings/Updates.svelte';
  import { desktopState } from '../stores/desktop.svelte';
  import { t } from '../stores/prefs.svelte';
  import { router } from '../stores/router.svelte';

  const SECTIONS = ['general', 'providers', 'system', 'engine'] as const;
  type Section = (typeof SECTIONS)[number];
  let picked = $state<Section | null>(null);
  // A named section in the address wins; any other name is a parameter, which lives under Engine.
  let fromLink = $derived<Section | null>(
    router.target ? ((SECTIONS as readonly string[]).includes(router.target) ? (router.target as Section) : 'engine') : null,
  );
  let section = $derived<Section>(picked ?? fromLink ?? 'general');
  $effect(() => {
    void router.target;
    picked = null;
  });
</script>

<div class="page-col">
  <div class="page-head">
    <h1>{t('nav.settings')}</h1>
  </div>
  <div class="tabs" role="tablist" aria-label={t('nav.settings')}>
    {#each SECTIONS as s (s)}
      <button type="button" role="tab" id="tab-{s}" aria-selected={section === s} aria-controls="panel-{s}" onclick={() => (picked = s)}>{t(`settings.section.${s}`)}</button>
    {/each}
  </div>
  <div class="panel" role="tabpanel" id="panel-{section}" aria-labelledby="tab-{section}">
    {#if section === 'general'}
      <Appearance />
      {#if desktopState.present}<AppSettings />{/if}
    {:else if section === 'providers'}
      <Providers />
    {:else if section === 'system'}
      <Backends />
      <Updates />
    {:else}
      <EngineSettings />
    {/if}
  </div>
</div>

<style>
  .tabs {
    display: flex;
    gap: var(--s1);
    border-bottom: var(--edge) solid var(--line);
    margin-bottom: var(--s5);
    overflow-x: auto;
  }
  .tabs button {
    border: none;
    box-shadow: none;
    background: none;
    border-radius: var(--radius) var(--radius) 0 0;
    color: var(--muted);
    border-bottom: 2px solid transparent;
    margin-bottom: -1px;
  }
  .tabs button:hover:not(:disabled) {
    color: var(--text);
    background: var(--hover);
  }
  .tabs button[aria-selected='true'] {
    color: var(--accent-ink);
    border-bottom-color: var(--accent);
  }
</style>
