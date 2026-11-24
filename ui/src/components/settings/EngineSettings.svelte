<script lang="ts">
  import { tick } from 'svelte';
  import ParamField from './ParamField.svelte';
  import { api } from '../../lib/api';
  import { commandLine } from '../../lib/format';
  import { decisionsByKnob, planDecisions } from '../../lib/plan';
  import { advancedCards, resettableKeys, setByYou, settingsCards } from '../../lib/schema';
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

  // What the person set is one card of its own, form or not, so it is all in view and resettable;
  // the group cards hold the rest of the form.
  let mine = $derived(setByYou(params, userKeys));
  let cards = $derived(settingsCards(params, userKeys));
  // Everything else a client may set, behind one fold and cut into the same groups: the plan sets
  // these, and a person who wants to choose (the devices that run the prompt and the answer, the
  // cache, the reads) finds them all here.
  let advanced = $derived(advancedCards(params, userKeys));
  const MINE = '__set_by_you';
  const ADVANCED = '__advanced';
  const adv = (group: string) => `__advanced:${group}`;

  // Open cards, keyed by group: every card starts closed, so the page opens on the groups alone.
  let openCards = $state<Record<string, boolean>>({});

  // A link to one setting (`#/settings/<key>`, from the Plan screen or a warning) lands on it: its
  // card opened (and Advanced settings, for a row there), the field scrolled into view and focused.
  // A row no card shows is not a target.
  // Once per link: the config changes on every save, and editing another field must not jump back.
  let landedOn = '';
  $effect(() => {
    const key = router.target;
    if (!key) landedOn = '';
    if (!key || key === landedOn || !config || params.length === 0) return;
    landedOn = key;
    const inAdvanced = advanced.find((c) => c.params.some((p) => p.key === key))?.group;
    const group = mine.some((p) => p.key === key)
      ? MINE
      : (cards.find((c) => c.params.some((p) => p.key === key))?.group ?? (inAdvanced && adv(inAdvanced)));
    if (!group) return;
    openCards = { ...openCards, [group]: true, ...(inAdvanced ? { [ADVANCED]: true } : {}) };
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
  {#if mine.length > 0}
    <details
      class="card group mine"
      open={openCards[MINE] === true}
      ontoggle={(e) => (openCards = { ...openCards, [MINE]: (e.currentTarget as HTMLDetailsElement).open })}
    >
      <summary>
        <span class="title">{t('settings.mine')}</span>
        <span class="badge">{mine.length}</span>
        <span class="muted ghelp">{t('settings.mine.help')}</span>
      </summary>
      {#each mine as spec (spec.key)}
        <ParamField
          {spec}
          value={config.values[spec.key]}
          userSet={true}
          rejected={settings.rejected[spec.key]}
          decision={decisions.get(spec.key)}
          needsReload={reloadKeys.has(spec.key)}
          readOnly={false}
        />
      {/each}
    </details>
  {/if}
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
  {#if advanced.length > 0}
    <details
      class="advanced"
      open={openCards[ADVANCED] === true}
      ontoggle={(e) => (openCards = { ...openCards, [ADVANCED]: (e.currentTarget as HTMLDetailsElement).open })}
    >
      <summary>
        <span class="title">{t('settings.advanced')}</span>
        <span class="badge">{advanced.reduce((n, c) => n + c.params.length, 0)}</span>
        <span class="muted ghelp">{t('settings.advanced.help')}</span>
      </summary>
      {#each advanced as c (c.group)}
        <details
          class="card group"
          open={openCards[adv(c.group)] === true}
          ontoggle={(e) => (openCards = { ...openCards, [adv(c.group)]: (e.currentTarget as HTMLDetailsElement).open })}
        >
          <summary>
            <span class="title">{c.label}</span>
            <span class="badge">{c.params.length}</span>
            {#if c.help}<span class="muted ghelp">{c.help}</span>{/if}
          </summary>
          {#each c.params as spec (spec.key)}
            <ParamField
              {spec}
              value={config.values[spec.key]}
              userSet={false}
              rejected={settings.rejected[spec.key]}
              decision={decisions.get(spec.key)}
              needsReload={reloadKeys.has(spec.key)}
              showLevel={true}
            />
          {/each}
        </details>
      {/each}
    </details>
  {/if}
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
  .advanced {
    margin-top: var(--s5);
  }
  .advanced > summary {
    cursor: pointer;
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: var(--s1) var(--s3);
    padding: var(--s2) 0;
    border-top: var(--edge) solid var(--line);
  }
  .advanced > summary .title {
    flex: 1;
  }
  .advanced > summary::after {
    order: 1;
    align-self: center;
  }
  .advanced[open] > summary {
    margin-bottom: var(--s3);
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
    order: 2;
  }
  /* The card's title takes the row, the count and the opening chevron (app.css) sit at its end,
     the help line under them. */
  .group > summary .title {
    flex: 1;
  }
  .group > summary::after {
    order: 1;
    align-self: center;
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
