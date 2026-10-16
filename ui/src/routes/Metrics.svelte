<script lang="ts">
  // Metrics: what the engine measured about its own work, for the current or last generation.
  // The records (the per-token CSV the server writes when launched with one) stay on disk, where
  // the launch option put them; this page is the live half.
  import MetricsPanel from '../components/MetricsPanel.svelte';
  import { server } from '../stores/server.svelte';
</script>

<h1>Metrics</h1>
{#if !server.info?.model}
  <p class="muted">No model loaded. Load one from Models; the figures appear while it generates.</p>
{:else if !server.info.model.streaming}
  <p class="banner info">
    This model loaded with streaming off (mmap baseline): it is read through the OS page cache, so per-token flash I/O,
    the compute split and cache hits are not observable in this mode.
  </p>
{/if}
<div class="wrap">
  <MetricsPanel />
</div>

<style>
  .wrap {
    max-width: 48rem;
  }
</style>
