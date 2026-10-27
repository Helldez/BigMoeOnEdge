<script lang="ts">
  // The loaded session's `streaming`, as the server reports it. Before a load there is nothing to
  // say: whether a model streams is decided at load (a dense model, a plan that declines).
  import { t } from '../stores/prefs.svelte';
  let { streaming }: { streaming: boolean } = $props();
</script>

{#if streaming}
  <span class="badge ok" title={t('streaming.on.hint')}>{t('streaming.on')}</span>
{:else}
  <span class="badge warn" title={t('streaming.off.hint')}>{t('streaming.off')}</span>
{/if}
