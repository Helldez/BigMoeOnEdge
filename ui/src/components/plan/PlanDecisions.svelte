<script lang="ts">
  // The decisions in the three groups a plan is read in: the settings it decided, open; the ones
  // it left, and the notes that are no setting (regime, memory use, storage speed...), folded. Every row opens to
  // its reason.
  import SourceBadge from '../SourceBadge.svelte';
  import { decisionLabel, groupDecisions } from '../../lib/plan';
  import { settingHref } from '../../lib/route';
  import type { PlanDecision } from '../../lib/types';

  let { decisions }: { decisions: PlanDecision[] } = $props();

  const groups = $derived(groupDecisions(decisions));
  const sections = $derived([
    { title: 'Set by the plan', hint: '', rows: groups.set, open: true },
    { title: 'Left as is', hint: 'Settings the plan looked at and left as they were.', rows: groups.kept, open: false },
    { title: 'Notes', hint: 'What the plan measured and weighed. None of these is a setting.', rows: groups.notes, open: false },
  ]);
</script>

{#each sections as s (s.title)}
  {#if s.rows.length}
    <details class="card group" open={s.open}>
      <summary>
        <span class="title">{s.title}</span>
        <span class="badge">{s.rows.length}</span>
        {#if s.hint}<span class="hint muted">{s.hint}</span>{/if}
      </summary>
      <!-- Keyed by position: a knob can carry several decisions (a note topic can recur), and a
           duplicate key stops Svelte rendering the whole page. -->
      {#each s.rows as d, i (i)}
        <details class="row">
          <summary>
            <span class="label">{decisionLabel(d)}</span>
            <span class="figure value">{d.value_text ?? (d.value && d.value !== '-' ? d.value : '')}</span>
            <SourceBadge source={d.source} reason={d.reason} />
          </summary>
          <div class="reason">
            <p>{d.brief || d.reason}</p>
            {#if d.brief && d.brief !== d.reason}
              <details class="why"><summary>Why</summary><p>{d.reason}</p></details>
            {/if}
            {#if d.label && d.group !== 'notes'}<a href={settingHref(d.knob)}>{d.label} in Settings</a>{/if}
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
    border-top: var(--edge) solid var(--border);
  }
  .row > summary {
    padding: 0.6rem 1rem;
    display: grid;
    grid-template-columns: minmax(0, 1fr) minmax(0, 10rem) 8.5rem;
  }
  .row > summary :global(.badge) {
    justify-self: end;
  }
  @media (max-width: 560px) {
    .row > summary {
      grid-template-columns: minmax(0, 1fr) auto;
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
    color: var(--accent);
    margin-bottom: 0.3rem;
  }
</style>
