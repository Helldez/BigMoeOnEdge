<script lang="ts">
  // The left column (docs/screens.md, "Layout"): the mark, New conversation, search, the
  // conversations by workspace and by date, and at the foot the loaded model and the way to every
  // other screen. Folded, it is a rail of icons; on a phone, a drawer.
  import Icon, { type IconName } from './Icon.svelte';
  import ModelCard from './ModelCard.svelte';
  import { timeMs, type ConversationSummary } from '../lib/agent';
  import { groupConversations } from '../lib/items';
  import { href, type Route } from '../lib/route';
  import { agent } from '../stores/agent.svelte';
  import { prefs, t } from '../stores/prefs.svelte';
  import { router } from '../stores/router.svelte';

  let { drawer = false, onNavigate = () => {} }: { drawer?: boolean; onNavigate?: () => void } = $props();

  let query = $state('');
  let renaming = $state('');
  let renameText = $state('');
  let confirmDelete = $state('');
  let collapsed = $derived(prefs.sidebarCollapsed && !drawer);
  let now = $state(Date.now());
  $effect(() => {
    const id = setInterval(() => (now = Date.now()), 60_000);
    return () => clearInterval(id);
  });
  let groups = $derived(groupConversations(agent.conversations, agent.workspaces, now, query));

  const NAV: { route: Route; icon: IconName; key: string }[] = [
    { route: 'models', icon: 'box', key: 'nav.models' },
    { route: 'plan', icon: 'gauge', key: 'nav.plan' },
    { route: 'skills', icon: 'spark', key: 'nav.skills' },
    { route: 'tasks', icon: 'clock', key: 'nav.tasks' },
    { route: 'metrics', icon: 'activity', key: 'nav.metrics' },
    { route: 'connect', icon: 'plug', key: 'nav.connect' },
    { route: 'settings', icon: 'gear', key: 'nav.settings' },
  ];

  function newConversation() {
    agent.newConversation();
    location.hash = '#/chat';
    onNavigate();
  }

  function startRename(c: ConversationSummary) {
    renaming = c.id;
    renameText = c.title;
  }
  async function commitRename(id: string) {
    const title = renameText.trim();
    renaming = '';
    if (title) await agent.rename(id, title);
  }
  async function remove(id: string) {
    if (confirmDelete !== id) {
      confirmDelete = id;
      return;
    }
    confirmDelete = '';
    await agent.remove(id);
  }

  const isOpen = (id: string) => router.route === 'chat' && agent.currentId === id;
  const stamp = (c: ConversationSummary) => {
    const ms = timeMs(c.updated);
    if (Number.isNaN(ms)) return '';
    const d = new Date(ms);
    const sameDay = new Date(now).toDateString() === d.toDateString();
    return new Intl.DateTimeFormat(prefs.lang, sameDay ? { hour: '2-digit', minute: '2-digit' } : { day: 'numeric', month: 'short' }).format(d);
  };
</script>

<nav class="side" class:collapsed aria-label={t('nav.label')}>
  <div class="brand-row">
    <a class="brand" href="#/chat" onclick={onNavigate} aria-label="BigMoeOnEdge">
      <img src="/logo.svg" alt="" width="24" height="24" />
      {#if !collapsed}<span>BigMoeOnEdge</span>{/if}
    </a>
    {#if !drawer}
      <button
        type="button"
        class="ghost icon"
        title={collapsed ? t('nav.expand') : t('nav.collapse')}
        aria-label={collapsed ? t('nav.expand') : t('nav.collapse')}
        aria-expanded={!collapsed}
        onclick={() => prefs.setSidebarCollapsed(!collapsed)}
      >
        <Icon name="sidebar" />
      </button>
    {/if}
  </div>

  <div class="actions">
    <button type="button" class="new" class:icon={collapsed} onclick={newConversation} title={t('conversation.new')}>
      <Icon name="plus" />
      {#if !collapsed}<span>{t('conversation.new')}</span>{/if}
    </button>
    {#if !collapsed}
      <label class="search">
        <Icon name="search" size={16} />
        <span class="visually-hidden">{t('conversation.search')}</span>
        <input type="search" placeholder={t('conversation.search')} bind:value={query} />
      </label>
    {:else}
      <button type="button" class="ghost icon" title={t('conversation.search')} aria-label={t('conversation.search')} onclick={() => prefs.setSidebarCollapsed(false)}>
        <Icon name="search" />
      </button>
    {/if}
  </div>

  {#if !collapsed}
    <div class="list" role="list" aria-label={t('conversation.list')}>
      <a class="entry tasks" href={href('tasks')} onclick={onNavigate} aria-current={router.route === 'tasks' ? 'page' : undefined}>
        <Icon name="clock" size={16} />
        <span>{t('nav.tasks')}</span>
      </a>
      {#if agent.listState === 'loading'}
        {#each [0, 1, 2, 3] as i (i)}<span class="skeleton row-skel"></span>{/each}
      {:else if agent.listState === 'unavailable'}
        <p class="note muted small">{t('conversation.unavailable')}</p>
      {:else if agent.listState === 'error'}
        <p class="note small err">{t('conversation.listFailed')} <button type="button" class="link" onclick={() => agent.refreshList()}>{t('action.retry')}</button></p>
      {:else if groups.length === 0}
        <p class="note muted small">{query ? t('conversation.noMatch') : t('conversation.none')}</p>
      {/if}
      {#each groups as g (g.key)}
        <div class="group" role="presentation">
          <h2 class="group-title" title={g.workspace?.path}>
            {#if g.workspace}<Icon name="folder" size={14} />{g.workspace.name}{:else}{t(`date.${g.bucket}`)}{/if}
          </h2>
          {#each g.items as c (c.id)}
            <div class="conv" role="listitem" class:open={isOpen(c.id)}>
              {#if renaming === c.id}
                <form
                  class="rename"
                  onsubmit={(e) => {
                    e.preventDefault();
                    commitRename(c.id);
                  }}
                >
                  <label class="visually-hidden" for="rename-{c.id}">{t('conversation.rename')}</label>
                  <!-- svelte-ignore a11y_autofocus -->
                  <input id="rename-{c.id}" type="text" bind:value={renameText} autofocus onblur={() => commitRename(c.id)} onkeydown={(e) => e.key === 'Escape' && (renaming = '')} />
                </form>
              {:else}
                <a class="entry" href="#/chat/{encodeURIComponent(c.id)}" onclick={onNavigate} aria-current={isOpen(c.id) ? 'page' : undefined}>
                  <span class="title">{c.title || t('conversation.untitled')}</span>
                  {#if c.state === 'running'}
                    <span class="dot info" title={t('state.running')}></span><span class="visually-hidden">{t('state.running')}</span>
                  {:else if c.state === 'waiting'}
                    <span class="badge warn">{t('state.waiting')}</span>
                  {:else if c.state === 'error'}
                    <span class="dot err" title={t('state.error')}></span><span class="visually-hidden">{t('state.error')}</span>
                  {:else}
                    <span class="when muted figure">{stamp(c)}</span>
                  {/if}
                </a>
                <span class="row-actions">
                  {#if confirmDelete === c.id}
                    <button type="button" class="small danger" onclick={() => remove(c.id)}>{t('action.delete')}</button>
                    <button type="button" class="small" onclick={() => (confirmDelete = '')}>{t('action.keep')}</button>
                  {:else}
                    <button type="button" class="ghost icon tiny" title={t('conversation.rename')} aria-label="{t('conversation.rename')}: {c.title}" onclick={() => startRename(c)}><Icon name="edit" size={15} /></button>
                    <button type="button" class="ghost icon tiny" title={t('action.delete')} aria-label="{t('action.delete')}: {c.title}" onclick={() => remove(c.id)}><Icon name="trash" size={15} /></button>
                  {/if}
                </span>
              {/if}
            </div>
          {/each}
        </div>
      {/each}
    </div>
  {:else}
    <div class="list"></div>
  {/if}

  <div class="foot">
    <ModelCard compact={collapsed} {onNavigate} />
    <ul class="nav" class:rail={collapsed}>
      {#each NAV as n (n.route)}
        <li>
          <a href={href(n.route)} onclick={onNavigate} aria-current={router.route === n.route ? 'page' : undefined} title={collapsed ? t(n.key) : undefined}>
            <Icon name={n.icon} size={18} />
            {#if collapsed}<span class="visually-hidden">{t(n.key)}</span>{:else}<span>{t(n.key)}</span>{/if}
          </a>
        </li>
      {/each}
    </ul>
  </div>
</nav>

<style>
  .side {
    display: flex;
    flex-direction: column;
    height: 100%;
    min-height: 0;
    background: var(--surface);
    border-right: var(--edge) solid var(--line);
    padding: var(--s3) var(--s2) var(--s2);
    gap: var(--s2);
    width: 100%;
  }
  .brand-row {
    display: flex;
    align-items: center;
    gap: var(--s2);
    padding: 0 var(--s1) 0 var(--s2);
    min-height: 2.25rem;
  }
  .collapsed .brand-row {
    flex-direction: column;
    padding: 0;
  }
  .brand {
    display: inline-flex;
    align-items: center;
    gap: var(--s2);
    flex: 1;
    min-width: 0;
    color: var(--text);
    text-decoration: none;
    font-weight: 650;
    font-size: 1rem;
    letter-spacing: -0.01em;
  }
  .brand img {
    image-rendering: pixelated;
    flex: none;
  }
  .actions {
    display: flex;
    flex-direction: column;
    gap: var(--s2);
  }
  .collapsed .actions {
    align-items: center;
  }
  .new {
    justify-content: flex-start;
  }
  .search {
    display: flex;
    align-items: center;
    gap: var(--s2);
    padding: 0 var(--s3);
    border: var(--edge) solid var(--line);
    border-radius: var(--radius);
    background: var(--bg);
    color: var(--muted);
  }
  .search:focus-within {
    border-color: var(--focus);
    outline: 2px solid var(--focus);
    outline-offset: 0;
  }
  .search input {
    border: none;
    background: transparent;
    padding: 0;
    min-height: 2.125rem;
    width: 100%;
    outline: none;
    color: var(--text);
  }
  .list {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    display: flex;
    flex-direction: column;
    gap: 2px;
    margin: 0 calc(-1 * var(--s1));
    padding: 0 var(--s1);
  }
  .group {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .group-title {
    display: flex;
    align-items: center;
    gap: var(--s1);
    margin: var(--s3) var(--s2) var(--s1);
    font-size: 0.75rem;
    font-weight: 600;
    color: var(--muted);
    letter-spacing: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .conv {
    position: relative;
    display: flex;
    align-items: center;
    border-radius: var(--radius);
  }
  .conv:hover,
  .conv:focus-within {
    background: var(--hover);
  }
  .conv.open {
    background: var(--accent-tint);
  }
  .entry {
    display: flex;
    align-items: center;
    gap: var(--s2);
    flex: 1;
    min-width: 0;
    min-height: 2.25rem;
    padding: 0 var(--s2);
    border-radius: var(--radius);
    color: var(--text);
    text-decoration: none;
    font-size: 0.875rem;
  }
  .entry.tasks {
    flex: none;
    color: var(--muted);
    font-weight: 550;
  }
  .entry.tasks:hover,
  .entry.tasks[aria-current='page'] {
    background: var(--hover);
    color: var(--text);
  }
  .title {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .when {
    font-size: 0.75rem;
  }
  .row-actions {
    display: none;
    gap: 2px;
    padding-right: var(--s1);
  }
  .conv:hover .row-actions,
  .conv:focus-within .row-actions {
    display: flex;
  }
  .conv:hover .when,
  .conv:focus-within .when {
    display: none;
  }
  button.tiny {
    width: 1.75rem;
    min-height: 1.75rem;
  }
  .rename {
    flex: 1;
    padding: 2px;
  }
  .rename input {
    width: 100%;
  }
  .note {
    padding: var(--s2);
    margin: 0;
  }
  .err {
    color: var(--err);
  }
  .row-skel {
    height: 1.75rem;
    margin: 2px var(--s2);
  }
  .foot {
    display: flex;
    flex-direction: column;
    gap: var(--s2);
    border-top: var(--edge) solid var(--line);
    padding-top: var(--s2);
  }
  .nav {
    list-style: none;
    margin: 0;
    padding: 0;
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 2px;
  }
  .nav.rail {
    grid-template-columns: 1fr;
    justify-items: center;
  }
  .nav a {
    display: flex;
    align-items: center;
    gap: var(--s2);
    min-height: 2.25rem;
    padding: 0 var(--s2);
    border-radius: var(--radius);
    color: var(--muted);
    text-decoration: none;
    font-size: 0.8125rem;
    font-weight: 550;
  }
  .nav.rail a {
    width: 2.25rem;
    justify-content: center;
    padding: 0;
  }
  .nav a:hover {
    background: var(--hover);
    color: var(--text);
  }
  .nav a[aria-current='page'] {
    background: var(--accent-tint);
    color: var(--accent-ink);
  }
  @media (pointer: coarse) {
    .row-actions {
      display: flex;
    }
    .when {
      display: none;
    }
    .entry,
    .nav a {
      min-height: 2.75rem;
    }
  }
</style>
