<script lang="ts">
  // The frame (docs/screens.md, "Layout"): the sidebar, the screen, and the preview pane beside a
  // conversation. Under 900 px the sidebar is a drawer behind a top bar; the preview covers the
  // screen when there is no room beside it.
  import { onMount, untrack } from 'svelte';
  import Icon from './components/Icon.svelte';
  import PreviewPane from './components/PreviewPane.svelte';
  import Sidebar from './components/Sidebar.svelte';
  import StatusBar from './components/StatusBar.svelte';
  import Connect from './routes/Connect.svelte';
  import Conversation from './routes/Conversation.svelte';
  import Models from './routes/Models.svelte';
  import Plan from './routes/Plan.svelte';
  import Settings from './routes/Settings.svelte';
  import Setup from './routes/Setup.svelte';
  import Skills from './routes/Skills.svelte';
  import Tasks from './routes/Tasks.svelte';
  import { api } from './lib/api';
  import { href } from './lib/route';
  import { agent } from './stores/agent.svelte';
  import { desktopState } from './stores/desktop.svelte';
  import { prefs, PREVIEW_MAX, PREVIEW_MIN } from './stores/prefs.svelte';
  import { t } from './lib/text';
  import { preview } from './stores/preview.svelte';
  import { router } from './stores/router.svelte';
  import { server } from './stores/server.svelte';

  let apiKey = $state('');
  let drawerOpen = $state(false);
  let width = $state(typeof window === 'undefined' ? 1280 : window.innerWidth);
  let narrow = $derived(width < 900);
  let previewBeside = $derived(width >= 1100 && !preview.fullscreen);

  onMount(() => {
    prefs.apply();
    router.start();
    server.start();
    agent.start();
    desktopState.start();
    // With nothing on disk there is nothing to talk to: a first visit (no screen named in the
    // address) opens the guided setup instead.
    if (!location.hash)
      api
        .models()
        .then((m) => {
          const downloading = m.catalog.some((e) => e.status === 'downloading');
          if (!location.hash && m.local.length === 0 && !downloading) location.hash = href('setup');
        })
        .catch(() => {});
  });

  // The address names the open conversation (`#/chat/<id>`); a bare `#/chat` is a new one.
  $effect(() => {
    const route = router.route;
    const id = router.target;
    untrack(() => {
      if (route === 'chat') agent.open(id);
      drawerOpen = false;
    });
  });

  // The preview belongs to a conversation: another one closes it.
  $effect(() => {
    const id = agent.currentId;
    untrack(() => {
      if (preview.open && preview.conversation !== id) preview.close();
    });
  });

  function submitKey(e: SubmitEvent) {
    e.preventDefault();
    if (!apiKey.trim()) return;
    server.submitKey(apiKey);
    agent.refreshList();
    apiKey = '';
  }

  // Dragging the edge between the conversation and the preview sets its width, remembered.
  function startResize(e: PointerEvent) {
    const handle = e.currentTarget as HTMLElement;
    handle.setPointerCapture(e.pointerId);
    const move = (ev: PointerEvent) => prefs.setPreviewWidth(window.innerWidth - ev.clientX);
    const up = () => {
      handle.removeEventListener('pointermove', move);
      handle.removeEventListener('pointerup', up);
    };
    handle.addEventListener('pointermove', move);
    handle.addEventListener('pointerup', up);
  }
  function resizeKey(e: KeyboardEvent) {
    const step = e.shiftKey ? 80 : 20;
    if (e.key === 'ArrowLeft') prefs.setPreviewWidth(prefs.previewWidth + step);
    else if (e.key === 'ArrowRight') prefs.setPreviewWidth(prefs.previewWidth - step);
    else return;
    e.preventDefault();
  }

  function globalKey(e: KeyboardEvent) {
    const mod = e.metaKey || e.ctrlKey;
    if (mod && e.key.toLowerCase() === 'k') {
      e.preventDefault();
      if (narrow) drawerOpen = true;
      else prefs.setSidebarCollapsed(false);
      queueMicrotask(() => document.querySelector<HTMLInputElement>('.side input[type="search"]')?.focus());
    } else if (e.key === 'Escape' && drawerOpen) {
      drawerOpen = false;
    }
  }

  let screenTitle = $derived(router.route === 'chat' ? agent.current?.title || t('conversation.new') : t(`nav.${router.route}`));
</script>

<svelte:window bind:innerWidth={width} onkeydown={globalKey} />

<a class="skip" href="#main">{t('nav.skip')}</a>

<div class="app" class:collapsed={prefs.sidebarCollapsed && !narrow} class:narrow>
  {#if narrow}
    <header class="topbar">
      <button type="button" class="ghost icon" aria-label={t('nav.open')} aria-expanded={drawerOpen} onclick={() => (drawerOpen = true)}><Icon name="menu" /></button>
      <span class="top-title">{screenTitle}</span>
      <a class="button ghost icon" href="#/chat" aria-label={t('conversation.new')} title={t('conversation.new')} onclick={() => agent.newConversation()}><Icon name="plus" /></a>
    </header>
    {#if drawerOpen}
      <!-- svelte-ignore a11y_click_events_have_key_events, a11y_no_static_element_interactions -->
      <div class="scrim" onclick={() => (drawerOpen = false)}></div>
      <div class="drawer"><Sidebar drawer onNavigate={() => (drawerOpen = false)} /></div>
    {/if}
  {:else}
    <aside class="sidebar"><Sidebar /></aside>
  {/if}

  <main id="main" class="main" tabindex="-1">
    <StatusBar />
    <div class="screen" class:scroll={router.route !== 'chat'}>
      {#if router.route === 'chat'}
        <Conversation />
      {:else if router.route === 'models'}
        <Models />
      {:else if router.route === 'settings'}
        <Settings />
      {:else if router.route === 'skills'}
        <Skills />
      {:else if router.route === 'tasks'}
        <Tasks />
      {:else if router.route === 'connect'}
        <Connect />
      {:else if router.route === 'setup'}
        <Setup />
      {:else}
        <Plan />
      {/if}
    </div>
  </main>

  {#if preview.open && router.route === 'chat'}
    {#if previewBeside}
      <!-- svelte-ignore a11y_no_noninteractive_tabindex, a11y_no_noninteractive_element_interactions -->
      <div
        class="handle"
        role="separator"
        aria-orientation="vertical"
        aria-label={t('preview.resize')}
        aria-valuemin={PREVIEW_MIN}
        aria-valuemax={PREVIEW_MAX}
        aria-valuenow={prefs.previewWidth}
        tabindex="0"
        onpointerdown={startResize}
        onkeydown={resizeKey}
      ></div>
      <div class="preview" style:width="{prefs.previewWidth}px"><PreviewPane /></div>
    {:else}
      <div class="preview overlay"><PreviewPane /></div>
    {/if}
  {/if}
</div>

{#if server.authRequired}
  <div class="veil" role="dialog" aria-modal="true" aria-labelledby="key-title">
    <form class="key card" onsubmit={submitKey}>
      <img src="/logo.svg" alt="" width="32" height="32" />
      <h2 id="key-title">{t('key.title')}</h2>
      <p class="muted">{t('key.help')}</p>
      <label class="visually-hidden" for="api-key">{t('key.label')}</label>
      <!-- svelte-ignore a11y_autofocus -->
      <input id="api-key" type="password" bind:value={apiKey} placeholder={t('key.label')} autocomplete="off" autofocus />
      <button type="submit" class="primary" disabled={!apiKey.trim()}>{t('key.connect')}</button>
    </form>
  </div>
{/if}

<style>
  .app {
    display: flex;
    height: 100vh;
    height: 100dvh;
    overflow: hidden;
  }
  .sidebar {
    flex: none;
    width: 17.5rem;
    height: 100%;
    transition: width var(--slow) var(--ease);
  }
  .collapsed .sidebar {
    width: 3.75rem;
  }
  .main {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
    height: 100%;
    outline: none;
  }
  .narrow {
    flex-direction: column;
  }
  .narrow .main {
    height: auto;
    flex: 1;
    min-height: 0;
  }
  .screen {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: column;
  }
  .screen.scroll {
    overflow-y: auto;
    display: block;
  }
  .topbar {
    display: flex;
    align-items: center;
    gap: var(--s2);
    min-height: 3rem;
    padding: 0 var(--s2);
    background: var(--surface);
    border-bottom: var(--edge) solid var(--line);
  }
  .top-title {
    flex: 1;
    min-width: 0;
    font-weight: 600;
    font-size: 0.9375rem;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .topbar :global(button.icon),
  .topbar a.icon {
    width: 2.75rem;
    min-height: 2.75rem;
    padding: 0;
  }
  .scrim {
    position: fixed;
    inset: 0;
    background: var(--scrim);
    z-index: 20;
  }
  .drawer {
    position: fixed;
    inset: 0 auto 0 0;
    width: min(20rem, 86vw);
    z-index: 21;
    box-shadow: var(--shadow-lg);
  }
  .handle {
    flex: none;
    width: 6px;
    margin: 0 -3px;
    z-index: 2;
    cursor: col-resize;
    background: transparent;
    transition: background var(--fast) var(--ease);
  }
  .handle:hover,
  .handle:focus-visible {
    background: var(--accent);
  }
  .preview {
    flex: none;
    height: 100%;
    min-width: 0;
  }
  .preview.overlay {
    position: fixed;
    inset: 0;
    width: auto;
    z-index: 30;
  }
  .skip {
    position: absolute;
    left: -999px;
    top: var(--s2);
    z-index: 50;
    padding: var(--s2) var(--s3);
    background: var(--surface);
    border-radius: var(--radius);
  }
  .skip:focus {
    left: var(--s2);
  }
  .veil {
    position: fixed;
    inset: 0;
    display: grid;
    place-items: center;
    background: var(--scrim);
    z-index: 60;
  }
  .key {
    display: flex;
    flex-direction: column;
    gap: var(--s3);
    width: min(24rem, calc(100vw - 2rem));
    padding: var(--s5);
    box-shadow: var(--shadow-lg);
  }
  .key img {
    image-rendering: pixelated;
  }
  .key h2,
  .key p {
    margin: 0;
  }
</style>
