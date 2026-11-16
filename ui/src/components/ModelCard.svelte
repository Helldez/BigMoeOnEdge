<script lang="ts">
  // The loaded model and what the engine is doing, at the foot of the sidebar (docs/screens.md,
  // "The status line"): a dot and the state in words, the model's name, a running download.
  // It leads to Models, where every one of these is acted on. Ready, it says whether the experts
  // stream from disk and the last answer's speed: the engine's own state, one glance from any
  // screen. `inline` is the phone's form, in the top bar: the dot and the state in one word.
  import { downloadSummary } from '../lib/library';
  import { href } from '../lib/route';
  import { server } from '../stores/server.svelte';
  import { t } from '../lib/text';

  let { compact = false, inline = false, onNavigate = () => {} }: { compact?: boolean; inline?: boolean; onNavigate?: () => void } = $props();

  let info = $derived(server.info);
  let downloads = $derived(downloadSummary(server.downloads));
  let tone = $derived(
    !server.connected ? 'err' : !info ? '' : info.calibrating ? 'warn' : info.state === 'ready' ? (info.generating ? 'info' : 'ok') : info.state === 'error' ? 'err' : info.state === 'empty' ? '' : 'warn',
  );
  let stateText = $derived(
    !server.connected
      ? t('status.unreachable')
      : !info
        ? t('status.connecting')
        : info.calibrating
          ? t('status.calibrating')
          : info.state === 'ready'
          ? info.generating
            ? t('status.generating')
            : t('status.ready')
          : t(`status.${info.state}`),
  );
  let title = $derived(info?.model?.name ?? (info?.state === 'empty' ? t('status.noModel') : ''));
  // Ready and idle: how the model runs and how fast it last wrote, in place of a bare "Ready".
  let detail = $derived.by(() => {
    const m = info?.model;
    if (!m || info?.state !== 'ready') return '';
    // A person reads that it is theirs and private; how it streams and how fast is on Plan.
    return m ? t('model.card.private') : '';
  });
</script>

{#if inline}
  <a class="inline" href={href('models')} onclick={onNavigate} aria-label="{stateText}{title ? `, ${title}` : ''}">
    <span class="dot {tone}"></span><span class="word">{stateText}</span>
  </a>
{:else}
<a class="card-link" class:compact href={href('models')} onclick={onNavigate} title={compact ? `${stateText}${title ? ` · ${title}` : ''}` : t('model.card.hint')}>
  <span class="dot {tone}"></span>
  {#if compact}
    <span class="visually-hidden">{stateText} {title}</span>
  {:else}
    <span class="text">
      <span class="name">{title || stateText}</span>
      <span class="state muted">
        {[title && title !== stateText ? stateText : '', detail].filter(Boolean).join(' · ')}
        {#if downloads}
          {(title && title !== stateText) || detail ? ' · ' : ''}<span class="figure">{t('model.card.downloading', { pct: downloads.fraction !== null ? Math.round(downloads.fraction * 100) : 0 })}</span>
        {/if}
      </span>
    </span>
  {/if}
</a>
{/if}

<style>
  .card-link {
    display: flex;
    align-items: center;
    gap: var(--s3);
    padding: var(--s2) var(--s3);
    border: var(--edge) solid var(--line);
    border-radius: var(--radius);
    background: var(--bg);
    color: var(--text);
    text-decoration: none;
    min-height: 3rem;
  }
  .card-link:hover {
    background: var(--hover);
  }
  .card-link.compact {
    justify-content: center;
    padding: 0;
    width: 2.25rem;
    min-height: 2.25rem;
    margin: 0 auto;
    border-radius: var(--pill);
  }
  .inline {
    display: inline-flex;
    align-items: center;
    gap: var(--s2);
    min-height: 44px;
    padding: 0 var(--s2);
    color: var(--muted);
    font-size: 0.8125rem;
    text-decoration: none;
    white-space: nowrap;
  }
  .text {
    display: flex;
    flex-direction: column;
    min-width: 0;
  }
  .name {
    font-size: 0.8125rem;
    font-weight: 600;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .state {
    font-size: 0.75rem;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
</style>
