<script lang="ts">
  // Every device the plan saw, one row each: its rate against the host, what it does in this run,
  // and why. A row is drawn the same for any device; the words are the server's, the colour follows
  // the role.
  import CellMeter from '../CellMeter.svelte';
  import { deviceTone, rateScale } from '../../lib/plan';
  import { t } from '../../lib/text';
  import type { PlanDevice } from '../../lib/types';

  let { devices, none = '' }: { devices: PlanDevice[]; none?: string } = $props();

  const scale = $derived(rateScale(devices));
</script>

<p class="muted caption">{t('plan.devices.caption')}</p>
<ul class="devices">
  {#each devices as d, i (i)}
    {@const tone = deviceTone(d)}
    <li>
      <div class="name">
        <!-- The device as the system names it first; the engine's id (what a setting takes) under it. -->
        {#if d.description && d.description !== d.name}
          <span class="dev">{d.description}</span>
          <span class="muted figure desc">{d.name}</span>
        {:else}
          <span class="figure dev">{d.name}</span>
        {/if}
      </div>
      <div class="rate">
        <span class="meter" class:idle={tone !== 'ok'}>
          <CellMeter value={d.prompt_ratio > 0 ? Math.max(5, (100 * d.prompt_ratio) / scale) : 0} label={t('plan.devices.meter', { name: d.description || d.name })} valueText={d.rate_text ?? ''} />
        </span>
        {#if d.rate_text}<span class="muted figure small">{d.rate_text}</span>{/if}
      </div>
      <div class="role">
        <span class="badge {tone}">{d.badge ?? d.status}</span>
        <span class="muted small">{d.why}</span>
      </div>
    </li>
  {/each}
</ul>
{#if none}
  <p class="muted none">{none}</p>
{/if}

<style>
  .devices {
    list-style: none;
    margin: 0;
    padding: 0;
  }
  li {
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem 1.25rem;
    padding: 0.75rem 0;
    border-top: var(--edge) solid var(--line);
  }
  li:first-child {
    border-top: none;
  }
  .name {
    flex: 1 1 180px;
    min-width: 0;
    display: flex;
    flex-direction: column;
  }
  .dev {
    font-size: 1.05rem;
  }
  .desc {
    font-size: 0.85rem;
    overflow-wrap: anywhere;
  }
  .rate {
    flex: 1 1 180px;
    display: flex;
    flex-direction: column;
    gap: 0.25rem;
  }
  .caption {
    margin: 0 0 var(--s2);
    font-size: 0.85rem;
  }
  /* A device the run does not use draws its rate in the quiet colour. */
  .meter.idle :global(i.on) {
    background: var(--line-strong);
  }
  .role {
    flex: 2 1 240px;
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 0.3rem;
  }
  .small {
    font-size: 0.85rem;
  }
  .none {
    margin: 0.5rem 0 0;
    border: var(--edge) dashed var(--line);
    padding: 0.4rem 0.6rem;
  }
</style>
