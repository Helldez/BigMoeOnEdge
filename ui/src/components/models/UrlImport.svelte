<!--
  Any other model: a direct https link to a gguf file. The server checks the link, reads the size,
  makes it a catalog entry (docs/server-api.md, "Models") and the download is then the catalog's,
  with the same progress, cancel and resume. Nothing here parses the link beyond trimming it: the
  server decides what it takes, and says why when it does not.
-->
<script lang="ts">
  import { api } from '../../lib/api';

  let { onAdded }: { onAdded: (id: string) => Promise<void> | void } = $props();

  let url = $state('');
  let busy = $state(false);
  let error = $state('');

  async function add(e: SubmitEvent) {
    e.preventDefault();
    busy = true;
    error = '';
    try {
      const r = await api.addUrl(url.trim());
      url = '';
      await onAdded(r.id);
    } catch (err) {
      error = err instanceof Error ? err.message : String(err);
    } finally {
      busy = false;
    }
  }
</script>

<form class="row" onsubmit={add}>
  <input type="url" bind:value={url} placeholder="https://.../model.gguf" aria-label="Direct link to a gguf file" autocomplete="off" />
  <button type="submit" class="primary" disabled={!url.trim() || busy}>{busy ? 'Checking the link...' : 'Download'}</button>
</form>
<p class="muted hint">A link to one file, or to any shard of a split set (the other shards are fetched beside it). It is added to Your models and downloads like any other.</p>
{#if error}<p class="banner error" role="alert">{error}</p>{/if}

<style>
  .row {
    display: flex;
    gap: 0.5rem;
    flex-wrap: wrap;
    align-items: center;
  }
  .row input {
    flex: 1 1 16rem;
    min-width: 0;
  }
  .hint {
    font-size: 0.85rem;
    margin: 0.4rem 0 0;
  }
</style>
