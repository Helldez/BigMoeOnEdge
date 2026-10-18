<script lang="ts">
  // The plan, read top down: what it decided in one sentence and how that turned out, where the
  // memory goes, what each device does, then the decisions grouped and the command. One layout for
  // every machine: each block is a list of whatever the plan holds.
  import { onMount } from 'svelte';
  import PlanDecisions from '../components/plan/PlanDecisions.svelte';
  import PlanDevices from '../components/plan/PlanDevices.svelte';
  import PlanMemory from '../components/plan/PlanMemory.svelte';
  import PlanProgress from '../components/plan/PlanProgress.svelte';
  import { api } from '../lib/api';
  import { commandLine, formatNum } from '../lib/format';
  import { speedCheck } from '../lib/plan';
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

  // A finished turn is a new measurement of the plan the model runs with: read it again (no probes).
  let seenDone: unknown = null;
  $effect(() => {
    const d = server.lastDone;
    if (d && d !== seenDone && plan?.available && plan.from_last_load && !measuring) {
      seenDone = d;
      refresh();
    }
  });

  const avail = $derived(plan?.available && !plan.error ? plan : null);
  const check = $derived(avail ? speedCheck(avail.predicted, avail.measured) : null);
  const showProgress = $derived(measuring || server.planProgress !== null);
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

{#if showProgress}
  <PlanProgress progress={server.planProgress} />
{/if}

{#if !plan}
  {#if !error && !showProgress}<p class="muted">Reading the plan...</p>{/if}
{:else if !plan.available}
  <div class="card">
    <p><strong>The automatic hardware planner is not in this build.</strong></p>
    <p class="muted">{plan.reason}</p>
    <p>Settings keep their defaults and the values you set yourself. Tune them in <a href="#/settings">Settings</a>.</p>
  </div>
{:else}
  <label class="auto">
    <input
      type="checkbox"
      checked={server.config?.auto_plan ?? false}
      disabled={savingAuto}
      onchange={(e) => setAuto((e.currentTarget as HTMLInputElement).checked)}
    />
    <span><strong>Plan before every load</strong>, on a quiet machine (the previous model is unloaded first). Values you set yourself are kept.</span>
  </label>

  {#if plan.error}
    <p class="muted">{plan.error}</p>
  {:else if avail && check}
    {#if avail.warning}
      <p class="banner warn" role="status">{avail.warning}</p>
    {/if}

    <section class="card summary">
      {#if avail.from_last_load}
        <span class="badge ok">The loaded model runs this plan</span>
      {/if}
      <p class="sentence">{avail.summary?.sentence ?? `Regime: ${avail.regime}`}</p>
      {#if avail.machine}<p class="muted machine">{avail.machine}</p>{/if}
      {#if avail.not_applicable?.length}
        <p class="warn-text">Not applicable in this build: {avail.not_applicable.join(', ')}</p>
      {/if}

      <div class="tiles">
        <div class="tile">
          <span class="muted k">Tokens</span>
          {#if check.measuredTokS !== null}
            <span><span class="figure big">{formatNum(check.measuredTokS)}</span> <span class="figure muted">tok/s measured</span></span>
            {#if check.deltaPct !== null && check.predictedTokS !== null}
              <span class="muted k"><span class="badge {check.deltaPct >= 0 ? 'ok' : 'warn'}">{check.deltaPct >= 0 ? '+' : ''}{check.deltaPct.toFixed(0)}%</span> against the {formatNum(check.predictedTokS)} expected</span>
            {/if}
          {:else if check.predictedTokS !== null}
            <span><span class="figure big">{formatNum(check.predictedTokS)}</span> <span class="figure muted">tok/s expected</span></span>
            <span class="muted k">{avail.from_last_load ? 'Measured after the first answer' : 'Measured after a load'}</span>
          {:else}
            <span class="muted">No prediction</span>
          {/if}
        </div>
        {#if check.predictedHitPct !== null || check.measuredHitPct !== null}
          <div class="tile">
            <span class="muted k">Cache hits</span>
            <span>
              <span class="figure big">{formatNum(check.measuredHitPct ?? check.predictedHitPct, 0)}%</span>
              <span class="figure muted">{check.measuredHitPct !== null ? 'measured' : 'credited'}</span>
            </span>
            <div class="hits" aria-hidden="true">
              <span style="width: {Math.min(100, check.measuredHitPct ?? check.predictedHitPct ?? 0)}%"></span>
              {#if check.measuredHitPct !== null && check.predictedHitPct !== null}
                <i style="left: {Math.min(100, check.predictedHitPct)}%"></i>
              {/if}
            </div>
            {#if check.measuredHitPct !== null && check.predictedHitPct !== null}
              <span class="muted k">The mark is the {check.predictedHitPct.toFixed(0)}% the plan credited</span>
            {/if}
          </div>
        {/if}
        {#if avail.summary?.lanes?.length}
          <div class="tile">
            <span class="muted k">Where it runs</span>
            {#each avail.summary.lanes as lane (lane.phase)}
              <div class="lane"><span>{lane.label ?? lane.phase}</span><span class="figure">{lane.on.join(' + ')}</span></div>
            {/each}
          </div>
        {/if}
      </div>

      <div class="apply">
        <button type="button" class="primary" onclick={apply} disabled={applying}>Apply plan</button>
        <span class="muted">Values you set yourself are never changed.</span>
      </div>
      {#if applied}<p class="ok-text" role="status">Plan applied. See the source badges in Settings.</p>{/if}
      {#if server.config?.reload_required && server.ready}
        <p class="muted">Some applied values need a model reload: use the banner in <a href="#/settings">Settings</a>.</p>
      {/if}
    </section>

    {#if avail.memory?.pools?.length}
      <h2>Memory</h2>
      <section class="card"><PlanMemory pools={avail.memory.pools} /></section>
    {/if}

    {#if avail.devices?.length}
      <h2>Devices</h2>
      <section class="card"><PlanDevices devices={avail.devices} none={avail.devices_none_text ?? ''} /></section>
    {/if}

    <h2>Decisions</h2>
    <PlanDecisions decisions={avail.decisions} />

    {#if avail.args?.length}
      <h2>Command</h2>
      <pre>{commandLine(avail.args)}</pre>
    {/if}

    {#if avail.explain}
      <details class="explain">
        <summary>The planner's full explanation</summary>
        <pre>{avail.explain}</pre>
      </details>
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
  .auto {
    display: flex;
    gap: 0.6rem;
    align-items: flex-start;
    margin-bottom: 1rem;
  }
  .auto input {
    margin-top: 0.25rem;
  }
  .summary {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 0.75rem;
  }
  .sentence {
    margin: 0;
    font-family: var(--pixel);
    font-size: 1.6rem;
    line-height: 1.2;
    max-width: 34ch;
  }
  .machine {
    margin: 0;
  }
  .tiles {
    align-self: stretch;
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
    gap: 0.75rem;
  }
  .tile {
    background: var(--bg);
    border: var(--edge) solid var(--border);
    padding: 0.75rem;
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
    min-width: 0;
  }
  .k {
    font-size: 0.85rem;
  }
  .big {
    font-size: 1.9rem;
    line-height: 1.1;
  }
  .hits {
    position: relative;
    height: 10px;
    border: var(--edge) solid var(--border);
    background: var(--surface-2);
  }
  .hits span {
    display: block;
    height: 100%;
    background: var(--accent);
  }
  .hits i {
    position: absolute;
    top: -5px;
    bottom: -5px;
    width: 2px;
    background: var(--text);
  }
  .lane {
    display: flex;
    justify-content: space-between;
    gap: 0.5rem;
    border: var(--edge) solid var(--border);
    padding: 0.2rem 0.5rem;
  }
  .lane .figure {
    color: var(--accent);
    text-align: right;
    overflow-wrap: anywhere;
  }
  .apply {
    display: flex;
    align-items: center;
    gap: 0.75rem;
    flex-wrap: wrap;
  }
  .warn-text {
    color: var(--warn);
    margin: 0;
  }
  .ok-text {
    color: var(--ok);
    margin: 0;
  }
  .explain summary {
    cursor: pointer;
    font-family: var(--pixel);
    margin: 1rem 0 0.5rem;
  }
  .explain pre {
    white-space: pre-wrap;
  }
</style>
