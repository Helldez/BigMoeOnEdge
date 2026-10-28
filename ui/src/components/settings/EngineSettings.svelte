<script lang="ts">
  import { tick } from 'svelte';
  import ParamField from './ParamField.svelte';
  import { api } from '../../lib/api';
  import { commandLine } from '../../lib/format';
  import { decisionsByKnob, planDecisions } from '../../lib/plan';
  import { resettableKeys, settingsCards } from '../../lib/schema';
  import { router } from '../../stores/router.svelte';
  import { server } from '../../stores/server.svelte';
  import { settings } from '../../stores/settings.svelte';
  import { t, tp } from '../../lib/text';

  let config = $derived(server.config);
  let params = $derived(server.params ?? []);
  let userKeys = $derived(new Set(config?.user_keys ?? []));
  let resettable = $derived(resettableKeys(config?.user_keys ?? [], config?.launch_keys ?? [], params));
  let reloadKeys = $derived(new Set(config?.reload_keys ?? []));
  let decisions = $derived(decisionsByKnob(planDecisions(config?.plan)));
  let command = $derived(config ? commandLine(config.args) : '');

  // A row the person set is shown even where the form leaves it out, so it can be seen and reset.
  let cards = $derived(settingsCards(params, userKeys));

  // Open cards, keyed by group: every card starts closed, so the page opens on the groups alone.
  let openCards = $state<Record<string, boolean>>({});

  // A link to one setting (`#/settings/<key>`, from the Plan screen or a warning) lands on it: its
  // card opened, the field scrolled into view and focused. A row no card shows is not a target.
  // Once per link: the config changes on every save, and editing another field must not jump back.
  let landedOn = '';
  $effect(() => {
    const key = router.target;
    if (!key) landedOn = '';
    if (!key || key === landedOn || !config || params.length === 0) return;
    landedOn = key;
    const card = cards.find((c) => c.params.some((p) => p.key === key));
    if (!card) return;
    openCards = { ...openCards, [card.group]: true };
    tick().then(() => {
      const el = document.getElementById(`param-${key}`);
      el?.scrollIntoView({ block: 'center' });
      el?.focus({ preventScroll: true });
    });
  });

  let copied = $state<'idle' | 'ok' | 'failed'>('idle');
  let reloadError = $state('');
  let confirmReset = $state(false);

  async function copy() {
    try {
      await navigator.clipboard.writeText(command);
      copied = 'ok';
    } catch {
      copied = 'failed';
    }
    setTimeout(() => (copied = copied === 'ok' ? 'idle' : copied), 2000);
  }

  async function reload() {
    reloadError = '';
    await settings.flush();
    try {
      await api.load();
    } catch (e) {
      reloadError = e instanceof Error ? e.message : String(e);
    }
  }

  async function resetAll() {
    confirmReset = false;
    await settings.flush();
    await settings.reset(...resettable);
  }

</script>

<div class="head">
  <p class="muted lead">{t('settings.engine.lead')}</p>
  <span class="muted saving" aria-live="polite">{settings.saving ? t('settings.saving') : ''}</span>
  {#if resettable.length > 0}
    {#if confirmReset}
      <span class="muted">{tp('settings.reset.confirm', resettable.length)}</span>
      <button type="button" class="primary" onclick={resetAll}>{t('settings.reset')}</button>
      <button type="button" onclick={() => (confirmReset = false)}>{t('action.keep')}</button>
    {:else}
      <button type="button" onclick={() => (confirmReset = true)}>{t('settings.reset.all')}</button>
    {/if}
  {/if}
  <button type="button" onclick={copy} disabled={!config}>{t('settings.copyCommand')}</button>
</div>

{#if copied === 'ok'}
  <p class="banner ok" role="status">{t('settings.copied')}</p>
{:else if copied === 'failed'}
  <div class="banner warn">
    <span>{t('settings.copyByHand')}</span>
    <pre class="cmd">{command}</pre>
  </div>
{/if}

{#if server.paramsError}
  <p class="banner error" role="alert">{t('settings.schemaFailed')} {server.paramsError}</p>
{/if}
{#if settings.requestError}
  <p class="banner error" role="alert">{t('settings.saveFailed')} {settings.requestError}</p>
{/if}
{#if config && !config.valid}
  <p class="banner error" role="alert">{t('settings.invalid', { error: config.error ? `: ${config.error}` : '' })}</p>
{/if}
{#if config?.reload_required && server.ready}
  <div class="banner warn">
    <span>{t('settings.reloadNeeded')}</span>
    <button type="button" class="primary" onclick={reload} disabled={!config.valid}>{t('settings.reload')}</button>
  </div>
{/if}
{#if reloadError}
  <p class="banner error" role="alert">{t('settings.reloadFailed')} {reloadError}</p>
{/if}

{#if !server.params || !config}
  <div class="skel" aria-hidden="true">{#each [0, 1, 2] as i (i)}<span class="skeleton card-skel"></span>{/each}</div>
{:else}
  {#each cards as c (c.group)}
    <details
      class="card group"
      open={openCards[c.group] === true}
      ontoggle={(e) => (openCards = { ...openCards, [c.group]: (e.currentTarget as HTMLDetailsElement).open })}
    >
      <summary>
        <span class="title" id="group-{c.group}">{c.label}</span>
        <span class="badge">{c.params.length}</span>
        {#if c.help}<span class="muted ghelp">{c.help}</span>{/if}
      </summary>
      {#each c.params as spec (spec.key)}
        <ParamField
          {spec}
          value={config.values[spec.key]}
          userSet={userKeys.has(spec.key)}
          rejected={settings.rejected[spec.key]}
          decision={decisions.get(spec.key)}
          needsReload={reloadKeys.has(spec.key)}
          readOnly={c.readOnly}
        />
      {/each}
    </details>
  {/each}
{/if}

<style>
  .head {
    display: flex;
    align-items: center;
    gap: var(--s2);
    flex-wrap: wrap;
    margin-bottom: var(--s4);
  }
  .lead {
    flex-basis: 100%;
    margin: 0 0 var(--s2);
  }
  .saving {
    margin-right: auto;
    font-size: 0.8125rem;
  }
  .group {
    margin-bottom: var(--s3);
  }
  .group > summary {
    cursor: pointer;
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: var(--s1) var(--s3);
  }
  .group[open] > summary {
    margin-bottom: var(--s2);
  }
  .title {
    font-size: 1.0625rem;
    font-weight: 650;
  }
  .ghelp {
    flex-basis: 100%;
    font-size: 0.8125rem;
  }
  .cmd {
    width: 100%;
    white-space: pre-wrap;
    overflow-wrap: anywhere;
    user-select: all;
    margin: 0;
  }
  .skel {
    display: flex;
    flex-direction: column;
    gap: var(--s3);
  }
  .card-skel {
    height: 7rem;
    border-radius: var(--radius);
  }
</style>
