<script lang="ts">
  import StreamingBadge from './StreamingBadge.svelte';
  import { server } from '../stores/server.svelte';

  const stateClass: Record<string, string> = { empty: '', loading: 'warn', ready: 'ok', error: 'err' };
  let info = $derived(server.info);
</script>

<div class="status" role="status" aria-live="polite">
  {#if !server.connected}
    <span class="badge err">Server unreachable, reconnecting</span>
  {/if}
  {#if info}
    <span class="badge {stateClass[info.state] ?? ''}">State: {info.state}</span>
    {#if info.state === 'error' && info.error}
      <span class="error-text">{info.error}</span>
    {/if}
    {#if info.model}
      <span class="model" title={info.model.path}>{info.model.name}</span>
      <span class="muted">{info.model.arch}</span>
      <StreamingBadge streaming={info.model.streaming} />
    {:else if info.state === 'empty'}
      <span class="muted">No model loaded</span>
      <StreamingBadge streaming={null} />
    {/if}
    {#if info.generating}
      <span class="badge info">Generating</span>
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
    border-bottom: 1px solid var(--border);
    background: var(--surface);
    font-size: 0.9rem;
  }
  .model {
    font-weight: 600;
    overflow-wrap: anywhere;
  }
  .error-text {
    color: var(--err);
    overflow-wrap: anywhere;
  }
  .version {
    margin-left: auto;
  }
</style>
