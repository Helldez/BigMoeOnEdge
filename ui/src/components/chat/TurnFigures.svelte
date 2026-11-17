<script lang="ts">
  // How a local answer ran (docs/screens.md, "The figures of an answer"): one quiet line under the
  // answer, tg (the writing speed) and pp (the prompt-reading speed), which opens to every figure in
  // plain words with what it measures, the speed against what the plan expected, and the way to the
  // plan and to Metrics. A disclosure rather than tooltips, so a finger and a keyboard reach it.
  import { formatNum } from '../../lib/format';
  import { speedCheck } from '../../lib/plan';
  import { href } from '../../lib/route';
  import { t } from '../../lib/text';
  import { turnFigures } from '../../lib/turnMetrics';
  import type { DoneSummary } from '../../lib/types';
  import { server } from '../../stores/server.svelte';

  let { summary, nCtx }: { summary: DoneSummary; nCtx: number } = $props();

  const expected = $derived.by(() => {
    const p = server.plan;
    if (!p?.available || p.error) return null;
    return speedCheck(p.predicted, p.measured).predictedTokS;
  });
  const figures = $derived(turnFigures(summary, nCtx, expected));
  const pp = $derived(summary.prefill_tps > 0 ? summary.prefill_tps : summary.n_prompt > 0 && summary.prefill_s > 0 ? summary.n_prompt / summary.prefill_s : 0);
</script>

<details class="turn">
  <summary class="figure">
    <span title={t('figure.speed')}>{t('figure.tg', { rate: formatNum(summary.tok_s, 1) })}</span>
    {#if pp > 0}<span title={t('figure.prompt')}>{t('figure.pp', { rate: formatNum(pp, 0) })}</span>{/if}
    <span class="visually-hidden">{t('figure.open')}</span>
  </summary>
  <dl>
    {#each figures as f (f.id)}
      <div>
        <dt class:stack={f.value.length > 24}>{f.label}{#if f.value}<span class="figure v">{f.value}</span>{/if}</dt>
        <dd>{f.hint}</dd>
      </div>
    {/each}
  </dl>
  <p class="links">
    <a href={href('plan')}>{t('figure.toPlan')}</a>
    <a href="{href('settings')}/metrics">{t('figure.toMetrics')}</a>
  </p>
</details>

<style>
  .turn {
    font-size: 0.8125rem;
  }
  /* Opened, it takes the row under the answer's actions instead of squeezing beside them. */
  .turn[open] {
    flex-basis: 100%;
  }
  summary {
    display: inline-flex;
    gap: var(--s3);
    color: var(--muted);
    font-weight: 550;
    padding: var(--s1) var(--s2);
    min-height: 28px;
  }
  summary:hover {
    color: var(--text);
  }
  dl {
    margin: var(--s2) 0 0;
    padding: var(--s3);
    background: var(--surface);
    border: var(--edge) solid var(--line);
    border-radius: var(--radius);
    box-shadow: var(--shadow-sm);
    display: grid;
    gap: var(--s3);
    max-width: 40rem;
  }
  /* The label, then the value at the right; a value too long for the row goes under the label. */
  dt {
    display: flex;
    flex-wrap: wrap;
    justify-content: space-between;
    gap: 0 var(--s3);
    font-weight: 700;
  }
  .v {
    color: var(--text);
    margin-left: auto;
    text-align: right;
  }
  /* A long value (the time split) reads as a line of its own under its label, not a ragged column. */
  .stack .v {
    flex-basis: 100%;
    margin-left: 0;
    text-align: left;
  }
  dd {
    margin: 2px 0 0;
    color: var(--muted);
  }
  .links {
    display: flex;
    gap: var(--s4);
    margin: var(--s2) 0 0;
  }
</style>
