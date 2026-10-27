<script lang="ts">
  import StreamingBadge from './StreamingBadge.svelte';
  import { contextAdvice, dismissContextWarning, dismissedContextWarning, warningText } from '../lib/context';
  import { downloadSummary } from '../lib/library';
  import { href } from '../lib/route';
  import { server } from '../stores/server.svelte';

  const stateClass: Record<string, string> = { empty: '', planning: 'warn', loading: 'warn', ready: 'ok', error: 'err' };
  const stateText: Record<string, string> = { planning: 'Measuring the machine, then loading' };
  let info = $derived(server.info);
  let ctx = $derived(contextAdvice(info?.model, server.params));
  let dismissed = $state(dismissedContextWarning());
  function dismiss(key: string) {
    dismissContextWarning(key);
    dismissed = key;
  }
  let downloads = $derived(downloadSummary(server.downloads));
</script>

<div class="status" role="status" aria-live="polite">
  {#if !server.connected}
    <span class="badge err">Server unreachable, reconnecting</span>
  {/if}
  {#if info}
    <span class="badge {stateClass[info.state] ?? ''}">State: {stateText[info.state] ?? info.state}</span>
    {#if info.state === 'error' && info.error}
      <span class="error-text">{info.error}</span>
    {/if}
    {#if info.model}
      <span class="model" title={info.model.path}>{info.model.name}</span>
      <span class="muted">{info.model.arch}</span>
      <StreamingBadge streaming={info.model.streaming} />
      {#if ctx?.warn && ctx.dismissKey !== dismissed}
        <span class="badge warn ctx" title="A coding agent's first request (system prompt and tools) needs about {ctx.recommended} tokens; a request that does not fit is refused.">
          {warningText(ctx)}
          <a href={ctx.href}>Raise {ctx.label}</a>
          <button type="button" class="dismiss" aria-label="Dismiss the context warning" title="Dismiss" onclick={() => ctx && dismiss(ctx.dismissKey)}>×</button>
        </span>
      {/if}
    {:else if info.state === 'empty'}
      <span class="muted">No model loaded</span>
    {/if}
    {#if info.generating}
      <span class="badge info">Generating</span>
    {/if}
    {#if downloads}
      <a class="badge info downloads" href={href('models')} title="Downloads run on the server; follow them in Models">
        {downloads.count === 1 ? 'Downloading' : `${downloads.count} downloads`}{downloads.fraction !== null ? ` ${(downloads.fraction * 100).toFixed(0)}%` : ''}
      </a>
    {/if}
    <span class="version muted">v{info.version}</span>
  {:else}
    <span class="muted">Connecting to server...</span>
  {/if}
</div>

<style>
  .status {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    flex-wrap: wrap;
    padding: 0.4rem 1rem;
    border-bottom: 1px solid var(--line);
    background: var(--surface);
    font-size: 0.9rem;
  }
  .model {
    font-weight: 600;
    overflow-wrap: anywhere;
  }
  .ctx {
    white-space: normal;
  }
  .dismiss {
    margin-left: 0.4rem;
    min-width: 1.75rem;
    min-height: 1.75rem;
    padding: 0;
    font-size: 1rem;
    line-height: 1;
  }
  .error-text {
    color: var(--err);
    overflow-wrap: anywhere;
  }
  .downloads {
    text-decoration: none;
  }
  .version {
    margin-left: auto;
  }
</style>
