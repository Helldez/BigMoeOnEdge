<script lang="ts">
  // The right pane: a file the agent produced or changed, read from the conversation's workspace
  // (`GET /api/conversations/<id>/file`). A page or a drawing runs in a sandboxed frame with no
  // access to this app (no same origin, the server's preview policy: docs/server-api.md, "Security
  // model"); Markdown is rendered and sanitised, code highlighted, images shown, text as text.
  import Icon from './Icon.svelte';
  import Markdown from './chat/Markdown.svelte';
  import { agentApi } from '../lib/agentApi';
  import { auth } from '../lib/auth';
  import { formatBytes } from '../lib/format';
  import { highlight } from '../lib/highlight';
  import { extOf, FRAMED, PREVIEW_TEXT_LIMIT, previewKind, type PreviewKind } from '../lib/preview';
  import { preview } from '../stores/preview.svelte';
  import { t } from '../stores/prefs.svelte';

  let kind = $state<PreviewKind>('text');
  let text = $state('');
  let src = $state('');
  let size = $state(0);
  let loading = $state(false);
  let error = $state('');
  let cut = $state(false);
  let objectUrl = '';

  function release() {
    if (objectUrl) URL.revokeObjectURL(objectUrl);
    objectUrl = '';
  }

  const asDataUrl = (b: Blob) =>
    new Promise<string>((resolve, reject) => {
      const r = new FileReader();
      r.onload = () => resolve(String(r.result));
      r.onerror = () => reject(r.error);
      r.readAsDataURL(b);
    });

  $effect(() => {
    const conv = preview.conversation;
    const path = preview.path;
    void preview.revision;
    if (!preview.open || !conv || !path) return;
    const ac = new AbortController();
    load(conv, path, ac.signal);
    return () => ac.abort();
  });

  async function load(conv: string, path: string, signal: AbortSignal) {
    loading = true;
    error = '';
    src = '';
    text = '';
    cut = false;
    release();
    try {
      const guess = previewKind(path);
      // Without a key, a page is framed straight from the server, under its preview policy, so its
      // own script runs. With one, the frame's request could not carry it: the bytes are fetched
      // here and framed from memory, where the app's policy holds and inline script does not run.
      if (FRAMED.has(guess) && !auth.key()) {
        kind = guess;
        src = `${agentApi.fileUrl(conv, path)}&r=${preview.revision}`;
        size = 0;
        return;
      }
      const f = await agentApi.file(conv, path, signal);
      size = f.blob.size;
      kind = previewKind(path, f.type);
      if (FRAMED.has(kind)) {
        objectUrl = URL.createObjectURL(f.blob);
        src = objectUrl;
      } else if (kind === 'image') {
        src = await asDataUrl(f.blob);
      } else if (kind !== 'binary') {
        cut = f.blob.size > PREVIEW_TEXT_LIMIT;
        text = await (cut ? f.blob.slice(0, PREVIEW_TEXT_LIMIT) : f.blob).text();
      }
    } catch (e) {
      if (signal.aborted) return;
      error = e instanceof Error ? e.message : String(e);
    } finally {
      if (!signal.aborted) loading = false;
    }
  }

  $effect(() => () => release());

  let name = $derived(preview.path.split('/').pop() ?? preview.path);
  let highlighted = $derived(kind === 'code' ? highlight(text, extOf(preview.path)) : '');
  function onkey(e: KeyboardEvent) {
    if (e.key === 'Escape' && preview.fullscreen) preview.fullscreen = false;
  }
</script>

<svelte:window onkeydown={onkey} />

<section class="pane" class:full={preview.fullscreen} aria-label={t('preview.label')}>
  <header>
    <Icon name="file" size={16} />
    <span class="name" title={preview.path}>{name}</span>
    <span class="path muted">{preview.path}</span>
    <span class="spacer"></span>
    <button type="button" class="ghost icon small" title={t('preview.refresh')} aria-label={t('preview.refresh')} onclick={() => preview.refresh()}><Icon name="refresh" size={16} /></button>
    <button type="button" class="ghost icon small" title={preview.fullscreen ? t('preview.exitFull') : t('preview.full')} aria-label={preview.fullscreen ? t('preview.exitFull') : t('preview.full')} aria-pressed={preview.fullscreen} onclick={() => (preview.fullscreen = !preview.fullscreen)}><Icon name="expand" size={16} /></button>
    <button type="button" class="ghost icon small" title={t('action.close')} aria-label={t('preview.close')} onclick={() => preview.close()}><Icon name="close" size={16} /></button>
  </header>
  <div class="body" aria-busy={loading}>
    {#if error}
      <div class="empty-state">
        <Icon name="alert" size={24} />
        <p>{t('preview.failed')}</p>
        <p class="small">{error}</p>
        <button type="button" onclick={() => preview.refresh()}>{t('action.retry')}</button>
      </div>
    {:else if loading && !src && !text}
      <div class="skel"><span class="skeleton"></span><span class="skeleton"></span><span class="skeleton short"></span></div>
    {:else if FRAMED.has(kind) && src}
      {#key src}
        <iframe title={t('preview.frame', { name })} {src} sandbox="allow-scripts" referrerpolicy="no-referrer"></iframe>
      {/key}
    {:else if kind === 'image' && src}
      <div class="image"><img {src} alt={name} /></div>
    {:else if kind === 'markdown'}
      <div class="doc"><Markdown {text} /></div>
    {:else if kind === 'code'}
      <pre class="code"><code>{@html highlighted}</code></pre>
    {:else if kind === 'binary'}
      <div class="empty-state"><Icon name="file" size={24} /><p>{t('preview.binary', { size: formatBytes(size) })}</p></div>
    {:else}
      <pre class="code">{text}</pre>
    {/if}
    {#if cut}<p class="cut muted small">{t('preview.cut', { shown: formatBytes(PREVIEW_TEXT_LIMIT), size: formatBytes(size) })}</p>{/if}
  </div>
</section>

<style>
  .pane {
    display: flex;
    flex-direction: column;
    height: 100%;
    min-width: 0;
    background: var(--surface);
    border-left: var(--edge) solid var(--line);
  }
  .pane.full {
    position: fixed;
    inset: 0;
    z-index: 40;
    border: none;
  }
  header {
    display: flex;
    align-items: center;
    gap: var(--s2);
    min-height: 3rem;
    padding: 0 var(--s2) 0 var(--s4);
    border-bottom: var(--edge) solid var(--line);
  }
  .name {
    font-weight: 600;
    font-size: 0.875rem;
    white-space: nowrap;
  }
  .path {
    font-family: var(--mono);
    font-size: 0.75rem;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    min-width: 0;
  }
  .body {
    flex: 1;
    min-height: 0;
    overflow: auto;
    display: flex;
    flex-direction: column;
  }
  iframe {
    flex: 1;
    width: 100%;
    border: none;
    /* A page's own canvas: what a browser draws behind a page that sets no background. */
    background: #ffffff;
  }
  .image {
    flex: 1;
    display: grid;
    place-items: center;
    padding: var(--s4);
    background: var(--surface-2);
  }
  .image img {
    max-width: 100%;
    max-height: 100%;
    border-radius: var(--radius-sm);
    box-shadow: var(--shadow-sm);
  }
  .doc {
    padding: var(--s4) var(--s5);
  }
  .code {
    margin: 0;
    border: none;
    border-radius: 0;
    flex: 1;
    font-size: 0.8125rem;
  }
  .skel {
    display: flex;
    flex-direction: column;
    gap: var(--s2);
    padding: var(--s4);
  }
  .skel .skeleton {
    height: 1rem;
  }
  .skel .short {
    width: 60%;
  }
  .cut {
    padding: var(--s2) var(--s4);
    margin: 0;
    border-top: var(--edge) solid var(--line);
  }
</style>
