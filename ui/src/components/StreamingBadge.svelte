<script lang="ts">
  // The loaded session's `streaming`, as the server reports it. Before a load there is nothing to
  // say: whether a model streams is decided at load (a dense model, a plan that declines).
  let { streaming }: { streaming: boolean } = $props();
</script>

{#if streaming}
  <span class="stream on" title="Experts are streamed from flash by the engine">Streaming ON</span>
{:else}
  <span class="stream off" title="Plain llama.cpp on mmap, without expert streaming">
    Streaming OFF (plain mmap baseline)
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
