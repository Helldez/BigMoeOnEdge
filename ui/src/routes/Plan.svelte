<script lang="ts">
  // The plan, read top down: what it decided in one sentence and how that turned out, where the
  // memory goes, what each device does, then the decisions grouped and the command. One layout for
  // every machine and the same one on every front-end (docs/screens.md): each block is a list of
  // whatever the plan holds. The plan itself lives in the server store, which reads it again when
  // a session opens and when a turn ends, so this page only draws it.
  import { onMount } from 'svelte';
  import PlanDecisions from '../components/plan/PlanDecisions.svelte';
  import PlanDevices from '../components/plan/PlanDevices.svelte';
  import PlanMemory from '../components/plan/PlanMemory.svelte';
  import PlanProgress from '../components/plan/PlanProgress.svelte';
  import { api } from '../lib/api';
  import { commandLine, formatBytes, formatNum } from '../lib/format';
  import { appliedChanges, diagnosticText, planDecisions, rateText, speedCheck, type AppliedChange } from '../lib/plan';
  import { href, settingHref } from '../lib/route';
  import { server } from '../stores/server.svelte';

  let error = $state('');
  let applying = $state(false);
  let applied = $state<AppliedChange[] | null>(null);

  async function measure() {
    error = '';
    applied = null;
    await server.measurePlan();
  }

  async function apply() {
    applying = true;
    applied = null;
    error = '';
    const before = { ...(server.config?.values ?? {}) };
    try {
      const cfg = await api.applyPlan();
      server.setConfig(cfg);
      applied = appliedChanges(before, cfg.values, server.params ?? []);
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

  // The store reads the plan when it reaches the server; a page opened before that asks once.
  onMount(() => {
    if (!server.plan && server.connected) server.refreshPlan();
  });

  const plan = $derived(server.plan);
  // While a model loads, the plan the server holds is the previous session's: this load's comes
  // when the session opens.
  const loading = $derived(server.info?.state === 'loading' || server.info?.state === 'planning');
  // The model the page is about: the one the shown plan measured, else the one a load would open.
  const modelPath = $derived(
    (plan?.available ? plan.model : '') || String(server.config?.values?.model ?? '') || server.info?.model?.path || '',
  );
  const modelName = $derived(modelPath.split(/[\\/]/).pop() ?? '');
  const measuring = $derived(server.measuring);
  const avail = $derived(plan?.available && !plan.error ? plan : null);
  const check = $derived(avail ? speedCheck(avail.predicted, avail.measured) : null);
  const showProgress = $derived(measuring || server.planProgress !== null);
  const host = $derived(avail?.memory?.pools?.[0]);
  // One banner for whatever went wrong: the request, Apply, or a plan the server could not make.
  const shownError = $derived(error || server.planError || (plan?.available ? (plan.error ?? '') : ''));
  // With nothing to show, what the settings carry from the last applied plan: never an empty page.
  const carried = $derived(planDecisions(server.config?.plan));
  const showCarried = $derived(!server.ready && carried.length > 0 && (!plan || !plan.available || plan.none || !!plan.error));

  // The planner's whole account stays in the API for a bug report; it is copied, not shown.
  let diag = $state<'idle' | 'ok' | 'failed'>('idle');
  async function copyDiagnostic() {
    if (!avail) return;
    try {
      await navigator.clipboard.writeText(diagnosticText(avail.args ?? [], avail.explain ?? ''));
      diag = 'ok';
    } catch {
      diag = 'failed';
    }
    setTimeout(() => (diag = diag === 'ok' ? 'idle' : diag), 2000);
  }
  const PLANNING_DOC = 'https://github.com/Helldez/BigMoeOnEdge/blob/main/docs/hardware-planning.md';
  import { t, tp } from '../lib/text';
</script>

<div class="page-col">
<div class="page-head">
  <h1>{t('nav.plan')}</h1>
  <p>{t('plan.lead')}</p>
  {#if modelName}
    <p class="for">{t('plan.for')} <strong title={modelPath}>{modelName}</strong> · <a href={href('models')}>{t('plan.change')}</a></p>
  {:else}
    <p class="for muted">{t('plan.noModel')}</p>
  {/if}
</div>

{#if shownError}
  <div class="banner error" role="alert">
    <span>{shownError}</span>
    <button type="button" onclick={measure} disabled={measuring}>{measuring ? t('plan.measuring') : t('plan.measureAgain')}</button>
  </div>
{/if}

{#if showProgress}
  <PlanProgress progress={server.planProgress} />
{/if}

{#if !plan}
  {#if !shownError && !showProgress}<div class="skel" aria-hidden="true"><span class="skeleton"></span><span class="skeleton"></span></div>{/if}
{:else if !plan.available}
  <div class="card">
    <p><strong>{t('plan.unavailable')}</strong></p>
    <p class="muted">{plan.reason}</p>
    <p>{t('plan.unavailable.help')} <a href="#/settings">{t('nav.settings')}</a>.</p>
  </div>
{:else}
  <label class="auto">
    <input
      type="checkbox"
      class="switch"
      role="switch"
      checked={server.config?.auto_plan ?? false}
      disabled={savingAuto}
      onchange={(e) => setAuto((e.currentTarget as HTMLInputElement).checked)}
    />
    <span><strong>{t('plan.auto')}</strong>{t('plan.auto.help')}</span>
  </label>

  {#if plan.none}
    <section class="card summary">
      <p class="sentence">{t('plan.notMeasured')}</p>
      <p class="muted machine">{t('plan.measureFor', { name: modelName || t('plan.theModel') })}</p>
      <div class="apply">
        <button type="button" class="primary" onclick={measure} disabled={measuring}>{measuring ? t('plan.measuring') : t('plan.measure')}</button>
      </div>
    </section>
  {:else if avail && check}
    {#if avail.warning}
      <p class="banner warn" role="status">{avail.warning}</p>
    {/if}

    {#if loading}
      <p class="muted" role="status">{t('plan.loading')}</p>
    {/if}
    <section class="card summary">
      {#if loading}
        <span class="badge">{t('plan.badge.before')}</span>
      {:else if avail.from_last_load}
        <span class="badge ok">{t('plan.badge.runs')}</span>
      {:else if avail.cached}
        <span class="badge">{t('plan.badge.last')}</span>
      {/if}
      <p class="sentence">{avail.summary?.sentence ?? t('plan.regime', { regime: avail.regime })}</p>
      {#if avail.machine}<p class="muted machine">{avail.machine}</p>{/if}
      {#if avail.summary?.lanes?.length}
        <p class="lanes">
          {#each avail.summary.lanes as lane (lane.phase)}
            <span><span class="muted">{lane.label ?? lane.phase}</span> <span class="figure">{lane.on.join(' + ')}</span></span>
          {/each}
        </p>
      {/if}
      {#if avail.not_applicable?.length}
        <p class="warn-text">{t('plan.notApplicable')} {avail.not_applicable.join(', ')}</p>
      {/if}

      <div class="tiles">
        <div class="tile">
          <span class="muted k">{t('plan.tile.tokens')}</span>
          {#if check.measuredTokS !== null}
            <span><span class="figure big">{rateText(check.measuredTokS)}</span> <span class="figure muted">{t('plan.tokMeasured')}</span></span>
            {#if avail.speed_check}
              <span class="muted k"><span class="badge {avail.speed_check.tone === 'ok' ? 'ok' : 'warn'}">{avail.speed_check.badge}</span> {avail.speed_check.against}</span>
            {:else if check.predictedTokS !== null}
              <span class="muted k">{t('plan.tokExpectedUnder', { rate: rateText(check.predictedTokS) })}</span>
            {/if}
          {:else if check.predictedTokS !== null}
            <span><span class="figure big">{rateText(check.predictedTokS)}</span> <span class="figure muted">{t('plan.tokExpected')}</span></span>
            <span class="muted k">{avail.from_last_load ? t('plan.afterAnswer') : t('plan.afterLoad')}</span>
          {:else}
            <span class="muted">{t('plan.noPrediction')}</span>
          {/if}
        </div>
        {#if check.predictedHitPct !== null || check.measuredHitPct !== null}
          <div class="tile">
            <span class="muted k">{t('plan.tile.hits')}</span>
            <span>
              <span class="figure big">{formatNum(check.measuredHitPct ?? check.predictedHitPct, 0)}%</span>
              <span class="figure muted">{check.measuredHitPct !== null ? t('plan.hits.measured') : t('plan.hits.credited')}</span>
            </span>
            <div class="hits" aria-hidden="true">
              <span style="width: {Math.min(100, check.measuredHitPct ?? check.predictedHitPct ?? 0)}%"></span>
              {#if check.measuredHitPct !== null && check.predictedHitPct !== null}
                <i style="left: {Math.min(100, check.predictedHitPct)}%"></i>
              {/if}
            </div>
            {#if check.measuredHitPct !== null && check.predictedHitPct !== null}
              <span class="muted k">{t('plan.hits.mark', { pct: check.predictedHitPct.toFixed(0) })}</span>
            {/if}
          </div>
        {/if}
        {#if host && host.cap_bytes > 0}
          <div class="tile">
            <span class="muted k">{t('plan.tile.memory')}</span>
            <span><span class="figure big">{formatBytes(host.held_bytes)}</span> <span class="figure muted">{t('plan.memory.held')}</span></span>
            <div class="hits" aria-hidden="true">
              <span style="width: {Math.min(100, (100 * host.held_bytes) / host.cap_bytes)}%"></span>
            </div>
            <span class="muted k">{t('plan.memory.ofCap', { size: formatBytes(host.cap_bytes) })}</span>
          </div>
        {/if}
      </div>

      <div class="apply">
        <button type="button" class="primary" onclick={apply} disabled={applying || measuring}>{t('plan.apply')}</button>
        <button type="button" onclick={measure} disabled={measuring}>
          {measuring ? t('plan.measuring') : t('plan.measureAgain')}
        </button>
        <span class="muted">{t('plan.apply.help')}</span>
      </div>
      {#if applied}
        <div class="applied" role="status">
          {#if applied.length === 0}
            <p class="ok-text">{t('plan.applied.none')}</p>
          {:else}
            <p class="ok-text">{tp('plan.applied', applied.length)}</p>
            <ul>
              {#each applied as c (c.key)}
                <li><a href={settingHref(c.key)}>{c.label}</a> <span class="muted figure">{c.from}</span> → <span class="figure">{c.to}</span></li>
              {/each}
            </ul>
          {/if}
        </div>
      {/if}
      {#if server.config?.reload_required && server.ready}
        <p class="muted">{t('plan.applied.reload')} <a href="#/settings">{t('nav.settings')}</a>.</p>
      {/if}
    </section>

    {#if avail.memory?.pools?.length}
      <h2>{t('plan.section.memory')}</h2>
      <section class="card"><PlanMemory pools={avail.memory.pools} /></section>
    {/if}

    {#if avail.devices?.length}
      <h2>{t('plan.section.devices')}</h2>
      <section class="card"><PlanDevices devices={avail.devices} none={avail.devices_none_text ?? ''} /></section>
    {/if}

    <h2>{t('plan.section.decisions')}</h2>
    <PlanDecisions decisions={avail.decisions} />

    {#if avail.args?.length}
      <h2>{t('plan.section.command')}</h2>
      <pre>{commandLine(avail.args)}</pre>
    {/if}
    <div class="apply diag">
      <button type="button" onclick={copyDiagnostic}>{t('plan.diagnostic')}</button>
      <span class="muted">{t('plan.diagnostic.help')}</span>
      {#if diag === 'ok'}<span class="ok-text" role="status">{t('plan.diagnostic.copied')}</span>{/if}
      <a class="doc" href={PLANNING_DOC} target="_blank" rel="noopener noreferrer">{t('plan.howItDecides')}</a>
    </div>
    {#if diag === 'failed'}
      <div class="banner warn">
        <span>{t('plan.clipboard')}</span>
        <pre class="cmd">{diagnosticText(avail.args ?? [], avail.explain ?? '')}</pre>
      </div>
    {/if}
  {/if}
{/if}

{#if showCarried}
  <h2>{t('plan.carried')}</h2>
  <p class="muted">{t('plan.carried.help')}</p>
  <PlanDecisions decisions={carried} />
{/if}

</div>

<style>
  .for {
    margin: var(--s2) 0 0;
    overflow-wrap: anywhere;
  }
  .auto {
    display: flex;
    gap: 0.6rem;
    align-items: flex-start;
    margin-bottom: 1rem;
  }
  .auto input {
    margin-top: 0.125rem;
  }
  .skel {
    display: flex;
    flex-direction: column;
    gap: var(--s3);
  }
  .skel .skeleton {
    height: 9rem;
    border-radius: var(--radius);
  }
  .summary {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 0.75rem;
  }
  .sentence {
    margin: 0;
    font-size: 1.375rem;
    font-weight: 600;
    letter-spacing: -0.01em;
    line-height: 1.25;
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
    border: var(--edge) solid var(--line);
    border-radius: var(--radius);
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
    height: 8px;
    border-radius: var(--pill);
    background: var(--surface-2);
  }
  .hits span {
    display: block;
    height: 100%;
    border-radius: var(--pill);
    background: var(--accent);
  }
  .hits i {
    position: absolute;
    top: -5px;
    bottom: -5px;
    width: 2px;
    background: var(--text);
  }
  .lanes {
    margin: 0;
    display: flex;
    flex-wrap: wrap;
    gap: 0.3rem 1.25rem;
  }
  .lanes .figure {
    color: var(--accent-ink);
    overflow-wrap: anywhere;
  }
  .doc {
    font-size: 0.9rem;
  }
  .applied ul {
    margin: 0.3rem 0 0;
    padding-left: 1.2rem;
  }
  .applied p {
    margin: 0;
  }
  .diag {
    margin-top: 0.75rem;
  }
  .cmd {
    width: 100%;
    white-space: pre-wrap;
    overflow-wrap: anywhere;
    user-select: all;
    margin: 0;
    max-height: 16rem;
    overflow: auto;
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
</style>
