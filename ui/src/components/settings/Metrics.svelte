<script lang="ts">
  // Metrics (docs/screens.md, "Metrics"), a section of Settings once switched on: what the engine
  // measured about its own work, live and for the last answer. A calm monitor: four figures with their recent history, where the time of a
  // token goes, and the last answer's account. The per-token CSV the server writes when launched
  // with one stays on disk, where the launch option put it.
  import MetricsPanel from '../MetricsPanel.svelte';
  import { href } from '../../lib/route';
  import { server } from '../../stores/server.svelte';
  import { t } from '../../lib/text';
</script>

<div>
  <p class="muted lead">{t('metrics.lead')}</p>
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

<style>
  .lead {
    margin: 0 0 var(--s4);
  }
</style>
