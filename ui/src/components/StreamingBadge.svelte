<script lang="ts">
  import { server } from '../stores/server.svelte';

  // `streaming` is the loaded session's truth; without a session, show what the config would do.
  let { streaming }: { streaming: boolean | null } = $props();
  let configured = $derived(server.config?.values['moe-stream']);
  let on = $derived(streaming ?? (typeof configured === 'boolean' ? configured : null));
</script>

{#if on === true}
  <span class="stream on" title="Experts are streamed from flash by the engine">
    Streaming ON{streaming === null ? ' (on load)' : ''}
  </span>
{:else if on === false}
  <span class="stream off" title="Plain llama.cpp on mmap, without expert streaming">
    Streaming OFF (plain mmap baseline){streaming === null ? ', on load' : ''}
  </span>
{/if}

<style>
  .stream {
    font-weight: 700;
    font-size: 0.85rem;
    padding: 0.15rem 0.6rem;
    border-radius: 999px;
    white-space: nowrap;
  }
  .on {
    background: var(--ok-bg);
    color: var(--ok);
  }
  .off {
    background: var(--warn-bg);
    color: var(--warn);
    border: 1px solid var(--warn);
  }
</style>
