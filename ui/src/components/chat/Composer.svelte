<script lang="ts">
  // Where a message is written (docs/screens.md, "Conversation"): the text, and the chips that say
  // where the agent works (a folder), what it may do there (a mode), and which model answers (the
  // local one or a saved provider). Enter sends, Shift+Enter starts a new line. The composer names
  // one engine parameter, `think`, which the chat itself needs (docs/screens.md).
  import Dialog from '../Dialog.svelte';
  import Icon from '../Icon.svelte';
  import Menu, { type MenuItem } from '../Menu.svelte';
  import { MODES, type Mode } from '../../lib/agent';
  import { desktop, isDesktop } from '../../lib/desktop';
  import { agent } from '../../stores/agent.svelte';
  import { server } from '../../stores/server.svelte';
  import { t } from '../../stores/prefs.svelte';

  let text = $state('');
  let options = $state('');
  let choose = $state(false);
  let think = $state<boolean | null>(null);
  let area: HTMLTextAreaElement | undefined = $state();
  let pathOpen = $state(false);
  let pathText = $state('');
  let pathError = $state('');

  let local = $derived(agent.route === 'local');
  let model = $derived(server.info?.model ?? null);
  let canThink = $derived(local && !!model && model.think_ctl !== 'none');
  let thinkDefault = $derived(server.config?.values?.think === false ? false : true);
  let thinking = $derived(think ?? thinkDefault);
  let localReady = $derived(server.ready);
  let blocked = $derived(local && !localReady);
  let canSend = $derived(!blocked && !agent.sending && !agent.running && text.trim() !== '' && (!choose || options.trim() !== ''));

  let ws = $derived(agent.workspaces.find((w) => w.id === agent.workspace) ?? null);
  let wsItems = $derived<MenuItem[]>([
    { value: '', label: t('workspace.none'), description: t('workspace.none.desc') },
    ...agent.workspaces.map((w) => ({ value: w.id, label: w.name, description: w.path })),
  ]);
  let modeItems = $derived<MenuItem[]>(
    MODES.map((m) => ({ value: m, label: t(`mode.${m}`), description: t(`mode.${m}.desc`), tone: m === 'full' ? ('warn' as const) : ('' as const) })),
  );
  let routeItems = $derived<MenuItem[]>([
    { value: 'local', label: model ? model.name : t('route.local'), description: model ? t('route.local.desc') : t('route.local.none') },
    ...agent.providers.flatMap((p) => p.models.map((m) => ({ value: `${p.id}/${m}`, label: m, description: t('route.provider.desc', { name: p.name }) }))),
  ]);
  let routeLabel = $derived(routeItems.find((r) => r.value === agent.route)?.label ?? agent.route);

  async function send() {
    if (!canSend) return;
    const msg = text;
    if (choose) {
      text = '';
      await agent.decide(msg.trim(), options);
      return;
    }
    text = '';
    const ok = await agent.send(msg, canThink ? thinking : undefined);
    if (!ok) text = msg;
    area?.focus();
  }

  function onKey(e: KeyboardEvent) {
    if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) {
      e.preventDefault();
      send();
    }
  }

  // The field grows with what is written, up to a third of the window, then scrolls.
  function autosize(node: HTMLTextAreaElement, _v: string) {
    const fit = () => {
      node.style.height = 'auto';
      node.style.height = `${Math.min(node.scrollHeight, window.innerHeight / 3)}px`;
    };
    fit();
    return { update: fit };
  }

  async function addFolder(close: () => void) {
    close();
    if (isDesktop()) {
      const path = await desktop.pickFolder();
      if (path) {
        const w = await agent.addWorkspace(path);
        if (w) agent.setWorkspace(w.id);
      }
      return;
    }
    pathText = '';
    pathError = '';
    pathOpen = true;
  }
  async function submitPath(e: SubmitEvent) {
    e.preventDefault();
    if (!pathText.trim()) return;
    const w = await agent.addWorkspace(pathText.trim());
    if (w) {
      pathOpen = false;
      agent.setWorkspace(w.id);
    } else {
      pathError = agent.actionError;
      agent.clearError();
    }
  }
</script>

<form
  class="composer"
  onsubmit={(e) => {
    e.preventDefault();
    send();
  }}
>
  {#if agent.mode === 'full' && agent.workspace}
    <p class="full-warning"><Icon name="alert" size={15} />{t('mode.full.warning')}</p>
  {/if}
  <div class="box" class:disabled={blocked}>
    <label class="visually-hidden" for="composer-text">{choose ? t('choose.question') : t('composer.label')}</label>
    <textarea
      id="composer-text"
      rows="1"
      bind:this={area}
      use:autosize={text}
      placeholder={blocked ? t('composer.placeholder.noModel') : choose ? t('choose.question.placeholder') : t('composer.placeholder')}
      bind:value={text}
      onkeydown={onKey}
    ></textarea>
    {#if choose}
      <label class="visually-hidden" for="composer-options">{t('choose.options')}</label>
      <textarea id="composer-options" class="options" rows="3" placeholder={t('choose.options.placeholder')} bind:value={options}></textarea>
    {/if}
    <div class="bar">
      <Menu items={wsItems} value={agent.workspace ?? ''} label={t('workspace.label')} onselect={(v) => agent.setWorkspace(v || null)}>
        {#snippet trigger()}
          <Icon name="folder" size={15} /><span class="chip-text">{ws ? ws.name : t('workspace.none.short')}</span>
        {/snippet}
        {#snippet footer(close)}
          <button type="button" onclick={() => addFolder(close)}><Icon name="plus" size={15} />{t('workspace.add')}</button>
        {/snippet}
      </Menu>
      <Menu items={modeItems} value={agent.mode} label={t('mode.label')} onselect={(v) => agent.setMode(v as Mode)}>
        {#snippet trigger()}
          <Icon name={agent.mode === 'full' ? 'alert' : agent.mode === 'read_only' ? 'eye' : 'shield'} size={15} /><span class="chip-text">{t(`mode.${agent.mode}`)}</span>
        {/snippet}
      </Menu>
      <Menu items={routeItems} value={agent.route} label={t('route.label')} onselect={(v) => agent.setRoute(v)}>
        {#snippet trigger()}
          <Icon name="chip" size={15} /><span class="chip-text">{routeLabel}</span>
        {/snippet}
      </Menu>
      {#if canThink}
        <button type="button" class="toggle" aria-pressed={thinking} title={t('composer.think.hint')} onclick={() => (think = !thinking)}>
          <Icon name="brain" size={15} />{t('composer.think')}
        </button>
      {/if}
      {#if local && localReady}
        <button type="button" class="toggle" aria-pressed={choose} title={t('choose.hint')} onclick={() => (choose = !choose)}>{t('choose.toggle')}</button>
      {/if}
      <span class="spacer"></span>
      {#if agent.running}
        <button type="button" class="stop" onclick={() => agent.stop()} title={t('composer.stop')}><Icon name="stop" size={16} />{t('composer.stop')}</button>
      {:else}
        <button type="submit" class="primary send icon" disabled={!canSend} aria-label={choose ? t('choose.send') : t('composer.send')} title={choose ? t('choose.send') : t('composer.send.hint')}>
          <Icon name="send" size={18} />
        </button>
      {/if}
    </div>
  </div>
  {#if blocked}
    <p class="hint muted small">{t('composer.noModel')} <a href="#/models">{t('nav.models')}</a></p>
  {/if}
</form>

<Dialog bind:open={pathOpen} title={t('workspace.add')}>
  <form id="path-form" onsubmit={submitPath}>
    <p class="muted">{t('workspace.path.help')}</p>
    <label for="ws-path">{t('workspace.path')}</label>
    <input id="ws-path" type="text" class="wide" placeholder="/home/me/project" bind:value={pathText} autocomplete="off" spellcheck="false" />
    {#if pathError}<p class="err small" role="alert">{pathError}</p>{/if}
  </form>
  {#snippet actions()}
    <button type="button" onclick={() => (pathOpen = false)}>{t('action.cancel')}</button>
    <button type="submit" form="path-form" class="primary" disabled={!pathText.trim()}>{t('workspace.add.confirm')}</button>
  {/snippet}
</Dialog>

<style>
  .composer {
    display: flex;
    flex-direction: column;
    gap: var(--s1);
  }
  .box {
    border: var(--edge) solid var(--line-strong);
    border-radius: calc(var(--radius) + 4px);
    background: var(--surface);
    box-shadow: var(--shadow-sm);
    padding: var(--s2) var(--s2) var(--s2) var(--s2);
    display: flex;
    flex-direction: column;
    gap: var(--s2);
  }
  .box:focus-within {
    border-color: var(--focus);
  }
  textarea {
    border: none;
    background: transparent;
    resize: none;
    width: 100%;
    min-height: 2.5rem;
    padding: var(--s1) var(--s2);
    outline: none;
  }
  textarea:focus-visible {
    outline: none;
  }
  .options {
    border-top: var(--edge) solid var(--line);
    border-radius: 0;
  }
  .bar {
    display: flex;
    align-items: center;
    gap: var(--s1);
    flex-wrap: wrap;
  }
  .toggle {
    min-height: 2rem;
    padding: 0 var(--s2);
    gap: var(--s1);
    border-radius: var(--pill);
    font-size: 0.8125rem;
    box-shadow: none;
  }
  .toggle[aria-pressed='true'] {
    background: var(--accent-tint);
    border-color: var(--accent);
    color: var(--accent-ink);
  }
  .send {
    border-radius: var(--pill);
  }
  .stop {
    border-radius: var(--pill);
  }
  .full-warning {
    display: flex;
    align-items: center;
    gap: var(--s2);
    margin: 0;
    padding: var(--s1) var(--s3);
    font-size: 0.8125rem;
    color: var(--warn);
    background: var(--warn-tint);
    border-radius: var(--radius);
  }
  .hint {
    margin: 0;
    padding: 0 var(--s2);
  }
  .wide {
    width: 100%;
    font-family: var(--mono);
  }
  .err {
    color: var(--err);
  }
  label[for='ws-path'] {
    display: block;
    font-weight: 550;
    margin-bottom: var(--s1);
  }
</style>
