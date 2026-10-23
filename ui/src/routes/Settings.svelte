<script lang="ts">
  import { tick } from 'svelte';
  import ParamField from '../components/settings/ParamField.svelte';
  import { api } from '../lib/api';
  import { commandLine } from '../lib/format';
  import { decisionsByKnob, planDecisions } from '../lib/plan';
  import { ALL_LEVELS, isDefault, levelCounts, levelOf, levelRank, resettableKeys, settingsCards } from '../lib/schema';
  import { router } from '../stores/router.svelte';
  import { server } from '../stores/server.svelte';
  import { settings } from '../stores/settings.svelte';

  // What a level means, in a line under its fold: the four levels are the table's enum, the rows in
  // them are the server's.
  const LEVEL_HINT: Record<string, string> = {
    advanced: 'Measured levers with a known trade-off.',
    experimental: 'Measured once or on one device: they can slow a run down.',
    debug: 'Tests, diagnostics and A/B baselines; never a speed setting.',
  };
  const LEVEL_KEY = 'bmoe.settings.level';

  function storedLevel(): string {
    try {
      const v = localStorage.getItem(LEVEL_KEY);
      if (v && (ALL_LEVELS as readonly string[]).includes(v)) return v;
    } catch {
      // No storage here: the default stands.
    }
    return 'advanced';
  }

  let config = $derived(server.config);
  let params = $derived(server.params ?? []);
  let userKeys = $derived(new Set(config?.user_keys ?? []));
  let resettable = $derived(resettableKeys(config?.user_keys ?? [], config?.launch_keys ?? [], params));
  let reloadKeys = $derived(new Set(config?.reload_keys ?? []));
  let decisions = $derived(decisionsByKnob(planDecisions(config?.plan)));
  let command = $derived(config ? commandLine(config.args) : '');
  let counts = $derived(levelCounts(params));

  let upTo = $state(storedLevel());
  let query = $state('');
  // A row someone or the plan set is shown whatever the filter: a value in force is never hidden.
  let pinned = $derived(
    new Set(
      params
        .filter((p) => userKeys.has(p.key) || decisions.has(p.key) || !isDefault(p, config?.values[p.key]))
        .map((p) => p.key),
    ),
  );
  let cards = $derived(settingsCards(params, { upTo, query, pinned }));
  let searching = $derived(query.trim() !== '');

  // Open state per card and per fold, keyed by group; a card with basic rows starts open, a card
  // that holds only deeper rows starts folded, so the page opens on the recommended configuration.
  let closedCards = $state<Record<string, boolean>>({});
  let openFolds = $state<Record<string, boolean>>({});

  function setLevel(level: string) {
    upTo = level;
    try {
      localStorage.setItem(LEVEL_KEY, level);
    } catch {
      // Remembered for this page only.
    }
  }

  // A link to one setting (`#/settings/<key>`, from the Plan screen or a warning) lands on it: the
  // filter widened to its level, its card and fold opened, the field scrolled into view and focused.
  // Once per link: the config changes on every save, and editing another field must not jump back.
  let landedOn = '';
  $effect(() => {
    const key = router.target;
    if (!key) landedOn = '';
    if (!key || key === landedOn || !config || params.length === 0) return;
    landedOn = key;
    const level = levelOf(params, key);
    if (!level) return;
    query = '';
    if (levelRank(level) > levelRank(upTo)) upTo = level;
    const group = params.find((p) => p.key === key)?.group ?? '';
    closedCards = { ...closedCards, [group]: false };
    if (level !== 'basic') openFolds = { ...openFolds, [group]: true };
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

  const levelName = (l: string) => l.charAt(0).toUpperCase() + l.slice(1);
</script>

<div class="head">
  <h1>Settings</h1>
  <span class="muted saving" aria-live="polite">{settings.saving ? 'Saving...' : ''}</span>
  {#if resettable.length > 0}
    {#if confirmReset}
      <span class="muted">Remove your {resettable.length} {resettable.length === 1 ? 'value' : 'values'}?</span>
      <button type="button" class="primary" onclick={resetAll}>Reset</button>
      <button type="button" onclick={() => (confirmReset = false)}>Keep</button>
    {:else}
      <button type="button" onclick={() => (confirmReset = true)}>Reset all my values</button>
    {/if}
  {/if}
  <button type="button" onclick={copy} disabled={!config}>Copy as command</button>
</div>

{#if copied === 'ok'}
  <p class="banner ok" role="status">Command copied.</p>
{:else if copied === 'failed'}
  <div class="banner warn">
    <span>The clipboard is not available here. Copy the command by hand:</span>
    <pre class="cmd">{command}</pre>
  </div>
{/if}

{#if server.paramsError}
  <p class="banner error" role="alert">Could not read the parameter schema: {server.paramsError}</p>
{/if}
{#if settings.requestError}
  <p class="banner error" role="alert">Saving failed: {settings.requestError}</p>
{/if}
{#if config && !config.valid}
  <p class="banner error" role="alert">This configuration does not validate{config.error ? `: ${config.error}` : ''}. Loading a model is refused until it is fixed.</p>
{/if}
{#if config?.reload_required && server.ready}
  <div class="banner warn">
    <span>Some changes only take effect after the model is reloaded.</span>
    <button type="button" class="primary" onclick={reload} disabled={!config.valid}>Reload model to apply</button>
  </div>
{/if}
{#if reloadError}
  <p class="banner error" role="alert">Reload failed: {reloadError}</p>
{/if}

{#if !server.params || !config}
  <p class="muted">Loading settings...</p>
{:else}
  <div class="tools">
    <input type="search" placeholder="Search settings" aria-label="Search settings" bind:value={query} />
    <div class="levels" role="radiogroup" aria-label="Show settings up to this level">
      {#each ALL_LEVELS as l (l)}
        <button
          type="button"
          role="radio"
          aria-checked={upTo === l}
          class:on={levelRank(l) <= levelRank(upTo)}
          disabled={searching}
          onclick={() => setLevel(l)}
        >
          {levelName(l)} <span class="muted">{counts.get(l) ?? 0}</span>
        </button>
      {/each}
    </div>
  </div>
  {#if searching}
    <p class="muted hint">{cards.length ? 'Every level is searched.' : 'No setting matches.'}</p>
  {/if}

  {#each cards as c (c.group)}
    {@const foldable = c.basic.length > 0 && c.more.length > 0}
    {@const moreCount = c.more.reduce((n, m) => n + m.params.length, 0)}
    <details
      class="card group"
      open={searching || (closedCards[c.group] ?? c.basic.length === 0) === false}
      ontoggle={(e) => {
        if (!searching) closedCards = { ...closedCards, [c.group]: !(e.currentTarget as HTMLDetailsElement).open };
      }}
    >
      <summary>
        <span class="title" id="group-{c.group}">{c.label}</span>
        {#if c.basic.length === 0}<span class="badge">{moreCount}</span>{/if}
        {#if c.help}<span class="muted ghelp">{c.help}</span>{/if}
      </summary>
      {#each c.basic as spec (spec.key)}
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
      {#if c.more.length}
        {#snippet levels()}
          {#each c.more as m (m.level)}
            <div class="level">
              <h3>{levelName(m.level)}</h3>
              {#if LEVEL_HINT[m.level]}<p class="muted hint">{LEVEL_HINT[m.level]}</p>{/if}
              {#each m.params as spec (spec.key)}
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
            </div>
          {/each}
        {/snippet}
        {#if foldable}
          <details
            class="more"
            open={searching || openFolds[c.group] === true}
            ontoggle={(e) => {
              if (!searching) openFolds = { ...openFolds, [c.group]: (e.currentTarget as HTMLDetailsElement).open };
            }}
          >
            <summary>{moreCount} more in {c.label}: {c.more.map((m) => m.level).join(', ')}</summary>
            {@render levels()}
          </details>
        {:else}
          {@render levels()}
        {/if}
      {/if}
      {#if c.hidden > 0 && !searching}
        <p class="muted hint hidden">{c.hidden} more at a deeper level: widen the filter above to see {c.hidden === 1 ? 'it' : 'them'}.</p>
      {/if}
    </details>
  {/each}
{/if}

<style>
  .head {
    display: flex;
    align-items: center;
    gap: 0.75rem;
    flex-wrap: wrap;
    margin-bottom: 0.75rem;
  }
  .head h1 {
    margin: 0;
  }
  .saving {
    margin-right: auto;
    font-size: 0.85rem;
  }
  .tools {
    display: flex;
    gap: 0.75rem;
    flex-wrap: wrap;
    align-items: center;
    margin-bottom: 0.75rem;
  }
  .tools input[type='search'] {
    flex: 1 1 14rem;
    min-width: 0;
  }
  .levels {
    display: inline-flex;
    flex-wrap: wrap;
    gap: 0.3rem;
  }
  .levels button.on {
    border-color: var(--accent);
  }
  .levels button[aria-checked='true'] {
    font-weight: 700;
  }
  .group {
    margin-bottom: 1rem;
  }
  .group > summary {
    cursor: pointer;
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: 0.3rem 0.75rem;
  }
  .group[open] > summary {
    margin-bottom: 0.5rem;
  }
  .title {
    font-size: 1.15rem;
    font-weight: 700;
  }
  .ghelp {
    flex-basis: 100%;
    font-size: 0.85rem;
  }
  .more {
    margin-top: 0.75rem;
  }
  .more > summary {
    cursor: pointer;
    color: var(--muted);
  }
  .level h3 {
    margin: 1rem 0 0;
    font-size: 0.95rem;
  }
  .hint {
    margin: 0.2rem 0 0.4rem;
    font-size: 0.85rem;
  }
  .hidden {
    margin-top: 0.6rem;
  }
  .cmd {
    width: 100%;
    white-space: pre-wrap;
    overflow-wrap: anywhere;
    user-select: all;
    margin: 0;
  }
</style>
