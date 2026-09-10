<script lang="ts">
  import LevelFilter from '../components/settings/LevelFilter.svelte';
  import ParamField from '../components/settings/ParamField.svelte';
  import { api } from '../lib/api';
  import { commandLine } from '../lib/format';
  import { decisionsByKnob, planDecisions } from '../lib/plan';
  import { countByLevel, groupParams } from '../lib/schema';
  import { server } from '../stores/server.svelte';
  import { settings } from '../stores/settings.svelte';

  let config = $derived(server.config);
  let params = $derived(server.params ?? []);
  let groups = $derived(groupParams(params, new Set(settings.levels)));
  let counts = $derived(countByLevel(params));
  let userKeys = $derived(new Set(config?.user_keys ?? []));
  let decisions = $derived(decisionsByKnob(planDecisions(config?.plan)));
  let command = $derived(config ? commandLine(config.args) : '');

  let copied = $state<'idle' | 'ok' | 'failed'>('idle');
  let reloadError = $state('');

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
</script>

<div class="head">
  <h1>Settings</h1>
  <span class="muted saving" aria-live="polite">{settings.saving ? 'Saving...' : ''}</span>
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

<LevelFilter {counts} />

{#if !server.params || !config}
  <p class="muted">Loading settings...</p>
{:else}
  {#each groups as g (g.group)}
    <section class="card group" aria-labelledby="group-{g.group}">
      <h2 id="group-{g.group}">{g.label}</h2>
      {#each g.params as spec (spec.key)}
        <ParamField
          {spec}
          value={config.values[spec.key]}
          userSet={userKeys.has(spec.key)}
          rejected={settings.rejected[spec.key]}
          decision={decisions.get(spec.key)}
        />
      {/each}
    </section>
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
  .group {
    margin-bottom: 1rem;
  }
  .group h2 {
    margin-top: 0;
  }
  .cmd {
    width: 100%;
    white-space: pre-wrap;
    overflow-wrap: anywhere;
    user-select: all;
    margin: 0;
  }
</style>
