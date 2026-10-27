<script lang="ts">
  // The loaded model and what the engine is doing, at the foot of the sidebar (docs/screens.md,
  // "The status line"): a dot and the state in words, the model's name, a running download.
  // It leads to Models, where every one of these is acted on.
  import { downloadSummary } from '../lib/library';
  import { href } from '../lib/route';
  import { server } from '../stores/server.svelte';
  import { t } from '../stores/prefs.svelte';

  let { compact = false, onNavigate = () => {} }: { compact?: boolean; onNavigate?: () => void } = $props();

  let info = $derived(server.info);
  let downloads = $derived(downloadSummary(server.downloads));
  let tone = $derived(
    !server.connected ? 'err' : !info ? '' : info.state === 'ready' ? (info.generating ? 'info' : 'ok') : info.state === 'error' ? 'err' : info.state === 'empty' ? '' : 'warn',
  );
  let stateText = $derived(
    !server.connected
      ? t('status.unreachable')
      : !info
        ? t('status.connecting')
        : info.state === 'ready'
          ? info.generating
            ? t('status.generating')
            : t('status.ready')
          : t(`status.${info.state}`),
  );
  let title = $derived(info?.model?.name ?? (info?.state === 'empty' ? t('status.noModel') : ''));
</script>

<a class="card-link" class:compact href={href('models')} onclick={onNavigate} title={compact ? `${stateText}${title ? ` · ${title}` : ''}` : t('model.card.hint')}>
  <span class="dot {tone}"></span>
  {#if compact}
    <span class="visually-hidden">{stateText} {title}</span>
  {:else}
    <span class="text">
      <span class="name">{title || stateText}</span>
      <span class="state muted">
        {title ? stateText : ''}{info?.model && !info.model.streaming ? ` · ${t('streaming.off')}` : ''}
        {#if downloads}
          {title ? ' · ' : ''}<span class="figure">{t('model.card.downloading', { pct: downloads.fraction !== null ? Math.round(downloads.fraction * 100) : 0 })}</span>
        {/if}
      </span>
    </span>
  {/if}
</a>

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
