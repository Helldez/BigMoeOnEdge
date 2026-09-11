<script lang="ts">
  import { onMount } from 'svelte';
  import SourceBadge from '../components/SourceBadge.svelte';
  import { api } from '../lib/api';
  import { commandLine } from '../lib/format';
  import type { Plan } from '../lib/types';
  import { server } from '../stores/server.svelte';

  let plan = $state<Plan | null>(null);
  let error = $state('');
  let applying = $state(false);
  let applied = $state(false);

  let measuring = $state(false);

  async function refresh(measure = false) {
    error = '';
    measuring = measure;
    try {
      plan = await api.plan(measure);
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
    } finally {
      measuring = false;
    }
  }

  async function apply() {
    applying = true;
    applied = false;
    error = '';
    try {
      server.setConfig(await api.applyPlan());
      applied = true;
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
    } finally {
      applying = false;
    }
  }

  let savingAuto = $state(false);
  async function setAuto(on: boolean) {
    savingAuto = true;
    error = '';
    try {
      server.setConfig(await api.updateConfig({ auto_plan: on }));
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
    } finally {
      savingAuto = false;
    }
  }

  onMount(() => refresh());
</script>

<div class="head">
  <h1>Plan</h1>
  <button type="button" onclick={() => refresh(true)} disabled={measuring}>
    {measuring ? 'Measuring...' : 'Measure again'}
  </button>
</div>

{#if error}
  <p class="banner error" role="alert">{error}</p>
{/if}

{#if !plan}
  {#if !error}<p class="muted">Measuring this machine (about 15 s)...</p>{/if}
{:else if !plan.available}
  <div class="card">
    <p><strong>The automatic hardware planner is not in this build.</strong></p>
    <p class="muted">{plan.reason}</p>
    <p>Settings keep their defaults and the values you set yourself. Tune them in <a href="#/settings">Settings</a>.</p>
  </div>
{:else}
  <div class="card summary">
    <label class="auto">
      <input
        type="checkbox"
        checked={server.config?.auto_plan ?? false}
        disabled={savingAuto}
        onchange={(e) => setAuto((e.currentTarget as HTMLInputElement).checked)}
      />
      <span><strong>Plan automatically</strong> every time a model loads, on a quiet machine (the previous model is unloaded first). Values you set yourself are kept.</span>
    </label>
  </div>

  {#if plan.error}
    <p class="muted">{plan.error}</p>
  {:else}
  {#if plan.warning}
    <p class="banner warn" role="status">{plan.warning}</p>
  {/if}
  <div class="card summary">
    {#if plan.from_last_load}
      <p class="ok-text">This is the plan the loaded model runs with, measured on a quiet machine when it loaded.</p>
    {/if}
    <p>Regime: <strong>{plan.regime}</strong></p>
    {#if plan.machine}<p class="muted">Machine: {plan.machine}</p>{/if}
    {#if plan.not_applicable?.length}
      <p class="warn-text">Not applicable in this build: {plan.not_applicable.join(', ')}</p>
    {/if}
    {#if plan.streaming_declined}
      <p class="warn-text">Streaming declined{plan.decline_reason ? `: ${plan.decline_reason}` : ''}.</p>
    {:else if plan.decline_reason}
      <p class="muted">{plan.decline_reason}</p>
    {/if}
    <div class="apply">
      <button type="button" class="primary" onclick={apply} disabled={applying}>Apply plan</button>
      <span class="muted">Values you set explicitly are never changed by the plan.</span>
    </div>
    {#if applied}<p class="ok-text" role="status">Plan applied. See the source badges in Settings.</p>{/if}
    {#if server.config?.reload_required && server.ready}
      <p class="muted">Some applied values need a model reload: use the banner in <a href="#/settings">Settings</a>.</p>
    {/if}
  </div>

  <h2>Decisions</h2>
  <div class="table-wrap">
    <table>
      <thead><tr><th>Parameter</th><th>Value</th><th>Source</th><th>Reason</th></tr></thead>
      <tbody>
        {#each plan.decisions as d (d.knob)}
          <tr>
            <td><code>{d.knob}</code></td>
            <td><code>{d.value}</code></td>
            <td><SourceBadge source={d.source} reason={d.reason} /></td>
            <td>{d.reason}</td>
          </tr>
        {/each}
      </tbody>
    </table>
  </div>

  {#if plan.args?.length}
    <h2>Command</h2>
    <pre>{commandLine(plan.args)}</pre>
  {/if}

  {#if plan.explain}
    <h2>Explanation</h2>
    <pre class="explain">{plan.explain}</pre>
  {/if}
  {/if}
{/if}

<style>
  .head {
    display: flex;
    align-items: center;
    gap: 0.75rem;
    margin-bottom: 0.75rem;
  }
  .head h1 {
    margin: 0;
  }
  .summary p {
    margin: 0.3rem 0;
  }
  .auto {
    display: flex;
    gap: 0.6rem;
    align-items: flex-start;
  }
  .auto input {
    margin-top: 0.25rem;
  }
  .apply {
    display: flex;
    align-items: center;
    gap: 0.75rem;
    flex-wrap: wrap;
    margin-top: 0.6rem;
  }
  .warn-text {
    color: var(--warn);
  }
  .ok-text {
    color: var(--ok);
  }
  .explain {
    white-space: pre-wrap;
  }
</style>
