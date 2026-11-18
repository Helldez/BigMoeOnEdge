<script lang="ts">
  // Where a message is written (docs/screens.md, "Conversation"): the text, and the chips that say
  // where the agent works (a folder), what it may do there (a mode), and which model answers (the
  // local one or a saved provider). What changes one message rather than the conversation (reason
  // first, attach an image, pick from options) sits behind one "+" menu, so the bar stays three
  // choices and Send. Enter sends, Shift+Enter starts a new line. The composer names one engine
  // parameter, `think`, which the chat itself needs (docs/screens.md).
  import Dialog from '../Dialog.svelte';
  import Icon from '../Icon.svelte';
  import Menu, { type MenuItem } from '../Menu.svelte';
  import { BUILTIN_AGENT, MODES, type Mode } from '../../lib/agent';
  import { desktop, isDesktop } from '../../lib/desktop';
  import { agent } from '../../stores/agent.svelte';
  import { server } from '../../stores/server.svelte';
  import { t } from '../../lib/text';

  let text = $state('');
  let options = $state('');
  // Images attached to the next message, as data URLs: shown as thumbnails until sent.
  let images = $state<string[]>([]);
  let imageError = $state('');
  let fileInput: HTMLInputElement | undefined = $state();
  const MAX_IMAGES = 8;
  const MAX_IMAGE_BYTES = 16 * 1024 * 1024;
  let choose = $state(false);
  let think = $state<boolean | null>(null);
  let area: HTMLTextAreaElement | undefined = $state();
  let pathOpen = $state(false);
  let pathText = $state('');
  let pathError = $state('');

  // Who does the work (GET /api/agent): offered where more than the built-in agent can run, and
  // chosen before the first message, since another agent would not know what was said.
  let agentChoices = $derived(agent.caps.agents);
  let chosen = $derived(agentChoices.find((a) => a.id === agent.agentId) ?? null);
  let external = $derived(agent.agentId !== BUILTIN_AGENT);
  let agentLocked = $derived(agent.items.length > 0);
  let agentItems = $derived<MenuItem[]>(
    agentChoices.map((a) => ({
      value: a.id,
      label: a.id === BUILTIN_AGENT ? t('agent.builtin') : a.name,
      description: !a.available
        ? t('agent.missing', { programs: (a.missing ?? []).join(', ') })
        : a.id === BUILTIN_AGENT
          ? t('agent.builtin.desc')
          : t('agent.external.desc'),
      tone: a.available ? ('' as const) : ('err' as const),
    })),
  );
  let agentLabel = $derived(chosen ? (chosen.id === BUILTIN_AGENT ? t('agent.builtin') : chosen.name) : t('agent.builtin'));
  let needsFolder = $derived(external && !agent.workspace);

  function pickAgent(id: string): void {
    const a = agentChoices.find((x) => x.id === id);
    if (!a || !a.available || agentLocked) return;
    agent.setAgent(id);
  }

  let local = $derived(agent.route === 'local');
  let model = $derived(server.info?.model ?? null);
  let canThink = $derived(local && !!model && model.think_ctl !== 'none');
  // The model reads images: a projector is loaded beside it (GET /api/info, model.vision). A saved
  // provider is asked as text only here.
  let canImage = $derived(local && !!model && !!model.vision && !choose);
  let thinkDefault = $derived(server.config?.values?.think === false ? false : true);
  let thinking = $derived(think ?? thinkDefault);
  let localReady = $derived(server.ready);
  // Why not, in one line under the button's title: the same three reasons the phone apps give.
  let imageWhyNot = $derived(
    !model || !localReady ? t('composer.image.noModel') : !model.vision ? t('composer.image.noVision') : choose ? t('composer.image.choose') : '',
  );
  let blocked = $derived(!external && local && !localReady);
  // The extras of one message, as switches and an action in the "+" menu.
  let extras = $derived<MenuItem[]>([
    ...(canThink ? [{ value: 'think', label: t('composer.think'), description: t('composer.think.hint'), on: thinking }] : []),
    ...(local
      ? [{ value: 'image', label: t('composer.image.add'), description: canImage ? t('composer.image.hint') : imageWhyNot, disabled: !canImage }]
      : []),
    ...(local && localReady ? [{ value: 'choose', label: t('choose.toggle'), description: t('choose.hint'), on: choose }] : []),
  ]);
  function extra(v: string): void {
    if (v === 'think') think = !thinking;
    else if (v === 'image') fileInput?.click();
    else if (v === 'choose') choose = !choose;
  }
  // A finger has no Enter key to learn about: the placeholder says so only where there is a keyboard.
  let coarse = $state(false);
  $effect(() => {
    const q = matchMedia('(pointer: coarse), (max-width: 640px)');
    coarse = q.matches;
    const on = () => (coarse = q.matches);
    q.addEventListener('change', on);
    return () => q.removeEventListener('change', on);
  });
  let canSend = $derived(
    !blocked &&
      !needsFolder &&
      !agent.sending &&
      !agent.running &&
      (text.trim() !== '' || (images.length > 0 && !choose)) &&
      (!choose || options.trim() !== ''),
  );

  // The first refusal is the one shown, as on the phones: a later file that is fine does not hide it.
  function addImageFiles(files: Iterable<File>): void {
    imageError = '';
    const refuse = (why: string) => {
      if (!imageError) imageError = why;
    };
    for (const f of files) {
      if (!f.type.startsWith('image/')) {
        refuse(t('composer.image.notImage'));
        continue;
      }
      if (f.size > MAX_IMAGE_BYTES) {
        refuse(t('composer.image.tooLarge', { mb: String(MAX_IMAGE_BYTES / (1024 * 1024)) }));
        continue;
      }
      if (images.length >= MAX_IMAGES) {
        refuse(t('composer.image.max', { n: String(MAX_IMAGES) }));
        break;
      }
      const reader = new FileReader();
      reader.onload = () => {
        if (typeof reader.result === 'string' && images.length < MAX_IMAGES) images = [...images, reader.result];
      };
      reader.readAsDataURL(f);
    }
  }
  function pickImages(e: Event) {
    const input = e.currentTarget as HTMLInputElement;
    if (input.files) addImageFiles(input.files);
    input.value = '';
  }
  function onPaste(e: ClipboardEvent) {
    if (!canImage || !e.clipboardData) return;
    const files = Array.from(e.clipboardData.items)
      .filter((i) => i.kind === 'file' && i.type.startsWith('image/'))
      .map((i) => i.getAsFile())
      .filter((f): f is File => !!f);
    if (files.length) {
      e.preventDefault();
      addImageFiles(files);
    }
  }
  function removeImage(i: number) {
    images = images.filter((_, k) => k !== i);
  }

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
  // The chip says the model's name without the file's extension: the part a person tells models by.
  let routeLabel = $derived((routeItems.find((r) => r.value === agent.route)?.label ?? agent.route).replace(/\.gguf$/i, ''));


  async function send() {
    if (!canSend) return;
    const msg = text;
    if (choose) {
      text = '';
      await agent.decide(msg.trim(), options);
      return;
    }
    text = '';
    const attached = images;
    images = [];
    const ok = await agent.send(msg, canThink ? thinking : undefined, attached.length ? attached : undefined);
    if (!ok) {
      text = msg;
      images = attached;
    }
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
      placeholder={blocked
        ? t('composer.placeholder.noModel')
        : choose
          ? t('choose.question.placeholder')
          : coarse
            ? t('composer.label')
            : t('composer.placeholder')}
      bind:value={text}
      onkeydown={onKey}
      onpaste={onPaste}
    ></textarea>
    {#if images.length}
      <ul class="attachments" aria-label={t('composer.image')}>
        {#each images as src, i (src + i)}
          <li>
            <img {src} alt="" />
            <button type="button" class="ghost icon small remove" aria-label={t('composer.image.remove')} title={t('composer.image.remove')} onclick={() => removeImage(i)}>
              <Icon name="close" size={13} />
            </button>
          </li>
        {/each}
      </ul>
    {/if}
    {#if imageError}<p class="err small" role="alert">{imageError}</p>{/if}
    {#if choose}
      <label class="visually-hidden" for="composer-options">{t('choose.options')}</label>
      <textarea id="composer-options" class="options" rows="3" placeholder={t('choose.options.placeholder')} bind:value={options}></textarea>
    {/if}
    <div class="bar">
      <!-- Who does the work is chosen before the first message; after it, the conversation is theirs. -->
      {#if agentChoices.length > 1 && !agentLocked}
        <Menu items={agentItems} value={agent.agentId} label={t('agent.label')} onselect={pickAgent}>
          {#snippet trigger()}
            <Icon name="spark" size={15} /><span class="chip-text squeeze">{agentLabel}</span>
          {/snippet}
        </Menu>
      {/if}
      {#if agent.caps.tools}
      <Menu items={wsItems} value={agent.workspace ?? ''} label={t('workspace.label')} onselect={(v) => agent.setWorkspace(v || null)}>
        {#snippet trigger()}
          <Icon name="folder" size={15} /><span class="chip-text squeeze">{ws ? ws.name : t('workspace.none.short')}</span>
        {/snippet}
        {#snippet footer(close)}
          <button type="button" onclick={() => addFolder(close)}><Icon name="plus" size={15} />{t('workspace.add')}</button>
        {/snippet}
      </Menu>
      <Menu items={modeItems} value={agent.mode} label={t('mode.label')} onselect={(v) => agent.setMode(v as Mode)}>
        {#snippet trigger()}
          <Icon name={agent.mode === 'full' ? 'alert' : agent.mode === 'read_only' ? 'eye' : 'shield'} size={15} /><span class="chip-text squeeze">{t(`mode.${agent.mode}`)}</span>
        {/snippet}
      </Menu>
      {/if}
      {#if !external}
        <span class="route"><Menu items={routeItems} value={agent.route} label={t('route.label')} onselect={(v) => agent.setRoute(v)}>
          {#snippet trigger()}
            <Icon name="chip" size={15} /><span class="chip-text">{routeLabel}</span>
          {/snippet}
        </Menu></span>
      {/if}
      {#if extras.length}
        <!-- Image is listed whenever the local model is the route, disabled with the reason while it
             reads no images: the feature is found before it can be used, as on the phones. -->
        {#if local}<input type="file" accept="image/*" multiple hidden bind:this={fileInput} onchange={pickImages} />{/if}
        <Menu items={extras} label={t('composer.more')} onselect={extra} switches chevron={false}>
          {#snippet trigger()}
            <Icon name="plus" size={16} /><span class="visually-hidden">{t('composer.more')}</span>
          {/snippet}
        </Menu>
        {#if choose}<span class="state">{t('choose.on')}</span>{/if}
        <!-- Said only when this message departs from the setting: a default is not news. -->
        {#if canThink && think !== null && think !== thinkDefault}<span class="state">{thinking ? t('composer.think') : t('composer.think.off')}</span>{/if}
      {/if}
      <span class="spacer"></span>
      {#if agent.running}
        <button type="button" class="stop" onclick={() => agent.stop()} title={t('composer.stop')}><Icon name="stop" size={16} /><span class="stop-text">{t('composer.stop')}</span></button>
      {:else}
        <button type="submit" class="primary send icon" disabled={!canSend} aria-label={choose ? t('choose.send') : t('composer.send')} title={choose ? t('choose.send') : t('composer.send.hint')}>
          <Icon name="send" size={18} />
        </button>
      {/if}
    </div>
  </div>
  {#if blocked}
    {#if server.info?.state === 'loading' || server.info?.state === 'planning'}
      <p class="hint muted small">{t('composer.loading')}</p>
    {:else}
      <p class="hint muted small">{t('composer.noModel')} <a href="#/models">{t('nav.models')}</a></p>
    {/if}
  {/if}
  {#if needsFolder}
    <p class="hint muted small">{t('agent.needsFolder', { name: agentLabel })}</p>
  {/if}
</form>

<Dialog bind:open={pathOpen} title={t('workspace.add')}>
  <form id="path-form" onsubmit={submitPath}>
    <p class="muted">{t('workspace.path.help')}</p>
    <label for="ws-path">{t('workspace.path')}</label>
    <input id="ws-path" type="text" class="wide" placeholder={t('workspace.path.example')} bind:value={pathText} autocomplete="off" spellcheck="false" />
    {#if pathError}<p class="err small" role="alert">{pathError}</p>{/if}
  </form>
  {#snippet actions()}
    <button type="button" onclick={() => (pathOpen = false)}>{t('action.cancel')}</button>
    <button type="submit" form="path-form" class="primary" disabled={!pathText.trim()}>{t('workspace.add.confirm')}</button>
  {/snippet}
</Dialog>

<style>
  /* The images going with the next message: small, removable, above the chips. */
  .attachments {
    display: flex;
    flex-wrap: wrap;
    gap: var(--s2);
    list-style: none;
    margin: 0;
    padding: 0 var(--s3) var(--s2);
  }
  .attachments li {
    position: relative;
  }
  .attachments img {
    display: block;
    width: 64px;
    height: 64px;
    object-fit: cover;
    border-radius: var(--radius);
    border: 1px solid var(--line);
  }
  .attachments .remove {
    position: absolute;
    top: -6px;
    right: -6px;
    background: var(--surface);
    border: 1px solid var(--line);
    border-radius: 50%;
  }
  /* A per-message extra that is on: said in words beside the "+", so it is not forgotten. */
  .state {
    font-size: 0.8125rem;
    color: var(--accent-ink);
    font-weight: 550;
  }
  .composer {
    display: flex;
    flex-direction: column;
    gap: var(--s1);
  }
  /* The composer: a white sheet, a fine ink edge, lifted like the answers. */
  .box {
    border: var(--edge) solid var(--line-strong);
    border-radius: var(--radius);
    background: var(--surface);
    box-shadow: var(--shadow-md);
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
  .send {
    border-radius: var(--pill);
  }
  @media (pointer: coarse), (max-width: 640px) {
    .send,
    .stop {
      min-height: 44px;
      min-width: 44px;
    }
  }
  /* A phone's width: one row, the model's chip taking what the icons leave, Send always last. */
  @media (max-width: 640px) {
    .bar {
      flex-wrap: nowrap;
    }
    .route {
      flex: 1 1 auto;
      min-width: 0;
      display: flex;
    }
    .route :global(.menu-root),
    .route :global(.chip) {
      min-width: 0;
      max-width: 100%;
    }
    /* The model's chip grows instead of the gap; without it (another agent) the gap pushes Send. */
    .route ~ .spacer {
      flex: 0 0 0;
    }
    .state {
      display: none;
    }
    /* Stop keeps its place, last and 44 px, as an icon; its word stays for a screen reader. */
    .stop {
      padding: 0;
      flex: none;
    }
    .stop-text {
      position: absolute;
      width: 1px;
      height: 1px;
      overflow: hidden;
      clip: rect(0 0 0 0);
      white-space: nowrap;
    }
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
