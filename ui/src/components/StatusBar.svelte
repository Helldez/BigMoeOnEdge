<script lang="ts">
  // What the engine's state asks a person to know, over every screen: an unreachable server, a load
  // under way or failed, a context too small for the clients the server serves. The everyday state
  // (the model, ready, generating, a download) lives in the sidebar's model card.
  import Icon from './Icon.svelte';
  import { contextAdvice, dismissContextWarning, dismissedContextWarning, warningText } from '../lib/context';
  import { href } from '../lib/route';
  import { server } from '../stores/server.svelte';
  import { t } from '../lib/text';

  let info = $derived(server.info);
  let ctx = $derived(contextAdvice(info?.model, server.params));
  let dismissed = $state(dismissedContextWarning());
  function dismiss(key: string) {
    dismissContextWarning(key);
    dismissed = key;
  }
</script>

<div class="notices" role="status" aria-live="polite">
  {#if !server.connected && server.everConnected}
    <p class="banner error"><Icon name="alert" size={16} />{t('status.unreachable.long')}</p>
  {:else if !server.connected && !info}
    <p class="banner info"><span class="skeleton spin"></span>{t('status.connecting')}</p>
  {/if}
  {#if info?.state === 'planning' || info?.state === 'loading'}
    <p class="banner info">
      <span class="skeleton spin"></span>{info.state === 'planning' ? t('status.planning.long') : t('status.loading.long')}
    </p>
  {:else if info?.state === 'error'}
    <p class="banner error">
      <Icon name="alert" size={16} /><span>{t('status.error.long')}{info.error ? `: ${info.error}` : ''}</span>
      <a class="button small" href={href('models')}>{t('status.error.next')}</a>
    </p>
  {/if}
  {#if ctx?.warn && ctx.dismissKey !== dismissed}
    <p class="banner warn ctx">
      <Icon name="info" size={16} />
      <span class="grow">{warningText(ctx)}</span>
      <a class="button small" href={ctx.href}>{t('context.raise', { label: ctx.label })}</a>
      <button type="button" class="ghost icon small" aria-label={t('context.dismiss')} title={t('action.dismiss')} onclick={() => ctx && dismiss(ctx.dismissKey)}>
        <Icon name="close" size={16} />
      </button>
    </p>
  {/if}
</div>

<style>
  .notices .banner {
    margin: var(--s3) var(--s4) 0;
    font-size: 0.875rem;
    padding: var(--s2) var(--s3);
  }
  .grow {
    flex: 1;
    min-width: 12rem;
  }
  .spin {
    width: 1rem;
    height: 1rem;
    min-height: 1rem;
    border-radius: 50%;
  }
</style>
