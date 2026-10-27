<script lang="ts">
  // Metrics (docs/screens.md, "Metrics"): what the engine measured about its own work, live and for
  // the last answer. A calm monitor: four figures with their recent history, where the time of a
  // token goes, and the last answer's account. The per-token CSV the server writes when launched
  // with one stays on disk, where the launch option put it.
  import MetricsPanel from '../components/MetricsPanel.svelte';
  import { href } from '../lib/route';
  import { server } from '../stores/server.svelte';
  import { t } from '../stores/prefs.svelte';
</script>

<div class="page-col">
  <div class="page-head">
    <h1>{t('nav.metrics')}</h1>
    <p>{t('metrics.lead')}</p>
  </div>
  {#if !server.info?.model}
    <div class="empty-state card">
      <h2>{t('metrics.noModel')}</h2>
      <p>{t('metrics.noModel.help')}</p>
      <a class="button primary" href={href('models')}>{t('nav.models')}</a>
    </div>
  {:else}
    {#if !server.info.model.streaming}
      <p class="banner info">{t('metrics.noStreaming')}</p>
    {/if}
    <MetricsPanel />
  {/if}
</div>
