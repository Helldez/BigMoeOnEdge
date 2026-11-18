<script lang="ts">
  // The decisions in the three groups a plan is read in: the settings it decided, open; the ones
  // it left, and the notes that are no setting (regime, memory use, storage speed...), folded. Every row opens to
  // its reason.
  import Icon from '../Icon.svelte';
  import SourceBadge from '../SourceBadge.svelte';
  import { decisionLabel, groupDecisions } from '../../lib/plan';
  import { settingHref } from '../../lib/route';
  import type { PlanDecision } from '../../lib/types';
  import { t } from '../../lib/text';

  let { decisions }: { decisions: PlanDecision[] } = $props();

  const groups = $derived(groupDecisions(decisions));
  const sections = $derived([
    { title: t('plan.group.set'), hint: '', rows: groups.set, open: true },
    { title: t('plan.group.kept'), hint: t('plan.group.kept.hint'), rows: groups.kept, open: false },
    { title: t('plan.group.notes'), hint: t('plan.group.notes.hint'), rows: groups.notes, open: false },
  ]);
</script>

{#each sections as s (s.title)}
  {#if s.rows.length}
    <details class="card group" open={s.open}>
      <summary class="own-marker">
        <span class="title">{s.title}</span>
        <span class="badge">{s.rows.length}</span>
        <span class="chev" aria-hidden="true"><Icon name="chevronDown" size={16} /></span>
        {#if s.hint}<span class="hint muted">{s.hint}</span>{/if}
      </summary>
      <!-- Keyed by position: a knob can carry several decisions (a note topic can recur), and a
           duplicate key stops Svelte rendering the whole page. -->
      {#each s.rows as d, i (i)}
        <details class="row">
          <summary class="own-marker">
            <span class="label">{decisionLabel(d)}</span>
            <span class="figure value">{d.value_text ?? (d.value && d.value !== '-' ? d.value : '')}</span>
            <span class="end"><SourceBadge source={d.source} reason={d.reason} /><span class="chev" aria-hidden="true"><Icon name="chevronDown" size={16} /></span></span>
          </summary>
          <div class="reason">
            <p>{d.brief || d.reason}</p>
            {#if d.brief && d.brief !== d.reason}
              <details class="why"><summary>{t('plan.why')}</summary><p>{d.reason}</p></details>
            {/if}
            {#if d.label && d.group !== 'notes'}<a href={settingHref(d.knob)}>{t('plan.inSettings', { label: d.label })}</a>{/if}
          </div>
        </details>
      {/each}
    </details>
  {/if}
{/each}

<style>
  .group {
    padding: 0;
    margin-bottom: 1rem;
  }
  summary {
    cursor: pointer;
    list-style: none;
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem 1rem;
  }
  summary::-webkit-details-marker {
    display: none;
  }
  .group > summary {
    padding: 0.75rem 1rem;
  }
  .title {
    flex: 1;
    font-size: 1.05rem;
  }
  .row {
    border-top: var(--edge) solid var(--line);
  }
  /* Label left; value, source and chevron one cluster on the right, so a row reads across. */
  .row > summary {
    padding: 0.6rem 1rem;
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto auto;
    gap: var(--s1) var(--s3);
  }
  .row > summary .value {
    text-align: right;
  }
  .end {
    justify-self: end;
    display: inline-flex;
    align-items: center;
    gap: var(--s2);
  }
  /* The disclosure's own marker is hidden for the layout; this one says the row opens. */
  .chev {
    display: inline-flex;
    color: var(--muted);
    transition: transform var(--fast) var(--ease);
  }
  details[open] > summary > .chev,
  details[open] > summary > .end > .chev {
    transform: rotate(180deg);
  }
  summary:hover {
    background: var(--hover);
  }
  /* A phone: the label with the source and chevron on its line, the value under the label. */
  @media (max-width: 560px) {
    .row > summary {
      grid-template-columns: minmax(0, 1fr) auto;
    }
    .row > summary .label {
      grid-column: 1;
      grid-row: 1;
    }
    .row > summary .end {
      grid-column: 2;
      grid-row: 1;
    }
    .row > summary .value {
      grid-column: 1 / -1;
      grid-row: 2;
      text-align: left;
      color: var(--muted);
    }
  }
  .row[open] > summary {
    background: var(--surface-2);
  }
  .label {
    flex: 1 1 200px;
    min-width: 0;
  }
  .value {
    flex: 0 1 160px;
    overflow-wrap: anywhere;
  }
  .reason {
    padding: 0.25rem 1rem 0.75rem;
    color: var(--muted);
    max-width: 80ch;
  }
  .reason p {
    margin: 0 0 0.4rem;
  }
  .hint {
    flex-basis: 100%;
    font-size: 0.85rem;
  }
  .why > summary {
    color: var(--accent-ink);
    margin-bottom: 0.3rem;
  }
</style>
