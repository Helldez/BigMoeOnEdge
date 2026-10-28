<script lang="ts">
  // The conversation (docs/screens.md, "Conversation"): the server's items as messages, tool cards
  // and notices, the composer under them. The run belongs to the server: this screen shows it as
  // it goes and sends what the person says or decides.
  import Icon from '../components/Icon.svelte';
  import Composer from '../components/chat/Composer.svelte';
  import LiveLine from '../components/chat/LiveLine.svelte';
  import MessageItem from '../components/chat/MessageItem.svelte';
  import RunGroup from '../components/chat/RunGroup.svelte';
  import { contextAdvice } from '../lib/context';
  import { blocks, latestFile } from '../lib/items';
  import { agent } from '../stores/agent.svelte';
  import { preview } from '../stores/preview.svelte';
  import { t, tp } from '../lib/text';
  import { server } from '../stores/server.svelte';

  let log: HTMLElement | undefined = $state();
  let view = $derived(blocks(agent.items));
  let lastAssistant = $derived.by(() => {
    for (let i = agent.items.length - 1; i >= 0; i--) if (agent.items[i].kind === 'assistant') return agent.items[i].id;
    return '';
  });
  let file = $derived(latestFile(agent.items));
  let ctx = $derived(contextAdvice(server.info?.model, server.params));
  let ws = $derived(agent.workspaces.find((w) => w.id === agent.workspace) ?? null);
  let title = $derived(agent.current?.title || (agent.currentId ? t('conversation.untitled') : t('conversation.new')));

  // Keep the newest words in view while they stream, unless the person scrolled up to read.
  let pinned = true;
  function onScroll() {
    if (!log) return;
    pinned = log.scrollHeight - log.scrollTop - log.clientHeight < 96;
  }
  $effect.pre(() => {
    void agent.items;
    void agent.decisions.length;
    if (!log || !pinned) return;
    queueMicrotask(() => log && (log.scrollTop = log.scrollHeight));
  });
  $effect(() => {
    void agent.currentId;
    pinned = true;
  });

  const errorKey: Record<string, string> = {
    busy: 'error.busy',
    no_model: 'error.noModel',
    local_only: 'error.localOnly',
    context_full: 'error.contextFull',
    not_a_folder: 'error.notAFolder',
  };
</script>

<div class="conversation">
  <header class="head">
    <h1 title={title}>{title}</h1>
    {#if ws}<span class="badge" title={ws.path}><Icon name="folder" size={13} />{ws.name}</span>{/if}
    {#if agent.current && agent.mode !== 'ask'}<span class="badge {agent.mode === 'full' ? 'warn' : ''}">{t(`mode.${agent.mode}`)}</span>{/if}
    <span class="spacer"></span>
    {#if file && !preview.open}
      <button type="button" class="ghost small" onclick={() => preview.show(agent.currentId, file)}><Icon name="eye" size={15} />{t('preview.show')}</button>
    {/if}
  </header>

  <div class="log" bind:this={log} onscroll={onScroll}>
    <div class="column">
      {#if agent.imported > 0}
        <p class="banner ok" role="status">
          {tp('conversation.imported', agent.imported)}
          <button type="button" class="ghost small" onclick={() => (agent.imported = 0)}>{t('action.dismiss')}</button>
        </p>
      {/if}
      {#if agent.currentError}
        <div class="banner error" role="alert">
          <span>{t('conversation.openFailed')} {agent.currentError}</span>
          <button type="button" onclick={() => agent.open(agent.currentId)}>{t('action.retry')}</button>
        </div>
      {:else if agent.loadingCurrent && agent.items.length === 0}
        <div class="skel" aria-hidden="true">
          <span class="skeleton user-skel"></span>
          <span class="skeleton"></span><span class="skeleton"></span><span class="skeleton short"></span>
        </div>
      {:else if agent.items.length === 0 && agent.decisions.length === 0}
        <div class="welcome">
          <img src="/logo.svg" alt="" width="40" height="40" />
          <h2>{t('conversation.welcome')}</h2>
          <p class="muted">{ws ? t('conversation.welcome.folder', { name: ws.name }) : t('conversation.welcome.plain')}</p>
        </div>
      {/if}

      {#each view as b (b.kind === 'run' ? `run:${b.id}` : b.item.id)}
        {#if b.kind === 'run'}
          <RunGroup items={b.items} conversation={agent.currentId} />
        {:else if b.item.kind === 'user' || b.item.kind === 'assistant' || b.item.kind === 'notice'}
          <MessageItem item={b.item} last={b.item.id === lastAssistant} />
        {/if}
      {/each}

      {#each agent.decisions as d (d.id)}
        <section class="decision card" aria-label={t('choose.result')}>
          <p class="q"><strong>{d.question}</strong></p>
          {#if d.pending}
            <p class="muted">{t('choose.pending')}</p>
          {:else if d.error}
            <p class="err">{d.error}</p>
          {:else if d.choices}
            <ul>
              {#each d.choices as c, i (c.label)}
                <li class:picked={i === d.best}>
                  <span class="label">{c.label}</span>
                  <span class="text">{c.text}</span>
                  <meter min="0" max="1" value={c.prob}></meter>
                  <span class="figure prob">{Math.round(100 * c.prob)}%</span>
                </li>
              {/each}
            </ul>
            {#if d.metrics}<p class="muted small figure">{d.metrics}</p>{/if}
          {/if}
          <button type="button" class="ghost small" onclick={() => agent.dismissDecision(d.id)}>{t('action.dismiss')}</button>
        </section>
      {/each}
    </div>
  </div>

  <div class="foot">
    <div class="column">
      {#if agent.actionError}
        <div class="banner error" role="alert">
          <span class="grow">{errorKey[agent.actionCode] ? t(errorKey[agent.actionCode]) : agent.actionError}</span>
          {#if agent.actionCode === 'no_model'}
            <a class="button small" href="#/models">{t('nav.models')}</a>
          {:else if agent.actionCode === 'context_full'}
            <a class="button small" href="#/chat" onclick={() => agent.newConversation()}>{t('conversation.new')}</a>
            {#if ctx}<a class="button small" href={ctx.href}>{t('context.raise', { label: ctx.label })}</a>{/if}
          {/if}
          <button type="button" class="ghost small" onclick={() => agent.clearError()}>{t('action.dismiss')}</button>
        </div>
      {/if}
      {#if agent.route === 'local' && agent.items.length > 0}<LiveLine />{/if}
      <Composer />
    </div>
  </div>
</div>

<style>
  .conversation {
    display: flex;
    flex-direction: column;
    height: 100%;
    min-height: 0;
  }
  .head {
    display: flex;
    align-items: center;
    gap: var(--s2);
    min-height: 3rem;
    padding: 0 var(--s4);
    border-bottom: var(--edge) solid var(--line);
  }
  .head h1 {
    margin: 0;
    font-size: 0.9375rem;
    font-weight: 600;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    min-width: 0;
  }
  /* On a phone the top bar already names the conversation. */
  @media (max-width: 899px) {
    .head h1 {
      display: none;
    }
  }
  .log {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
  }
  .column {
    width: 100%;
    max-width: 48rem;
    margin: 0 auto;
    padding: 0 var(--s4);
  }
  .log .column {
    display: flex;
    flex-direction: column;
    gap: var(--s4);
    padding-top: var(--s5);
    padding-bottom: var(--s5);
  }
  .foot .column {
    display: flex;
    flex-direction: column;
    gap: var(--s2);
    padding-bottom: var(--s4);
  }
  .foot .banner {
    margin: 0;
  }
  .grow {
    flex: 1;
    min-width: 12rem;
  }
  .welcome {
    display: flex;
    flex-direction: column;
    align-items: center;
    text-align: center;
    gap: var(--s2);
    margin: 12vh auto 0;
    max-width: 30rem;
  }
  .welcome img {
    image-rendering: pixelated;
  }
  .welcome h2 {
    margin: var(--s2) 0 0;
    font-size: 1.375rem;
  }
  .skel {
    display: flex;
    flex-direction: column;
    gap: var(--s2);
  }
  .skel .skeleton {
    height: 1rem;
  }
  .skel .user-skel {
    align-self: flex-end;
    width: 40%;
    height: 2.5rem;
    margin-bottom: var(--s3);
  }
  .skel .short {
    width: 55%;
  }
  .decision ul {
    list-style: none;
    margin: var(--s2) 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: var(--s1);
  }
  .decision li {
    display: grid;
    grid-template-columns: 1.5rem minmax(0, 1fr) 6rem 3rem;
    align-items: center;
    gap: var(--s2);
  }
  .decision li.picked {
    font-weight: 650;
  }
  .decision .text {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .decision meter {
    width: 100%;
  }
  .decision .prob {
    text-align: right;
  }
  .q {
    margin: 0;
  }
  .err {
    color: var(--err);
  }
</style>
