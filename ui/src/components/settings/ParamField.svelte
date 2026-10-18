<script lang="ts">
  import { untrack } from 'svelte';
  import SourceBadge from '../SourceBadge.svelte';
  import { coerceInput, displayValue } from '../../lib/coerce';
  import { helpParts, isDefault, planSuggestion, specialLabel, typicalText } from '../../lib/schema';
  import type { ParamSpec, ParamValue, PlanDecision } from '../../lib/types';
  import { settings } from '../../stores/settings.svelte';

  let {
    spec,
    value,
    userSet,
    rejected = '',
    decision,
  }: { spec: ParamSpec; value: ParamValue | undefined; userSet: boolean; rejected?: string; decision?: PlanDecision } =
    $props();

  const id = $derived(`param-${spec.key}`);
  let draft = $state('');
  let localError = $state('');
  let focused = false;

  // Follow the server's value, except while the user is typing in this field.
  $effect(() => {
    const v = displayValue(value);
    untrack(() => {
      if (!focused) {
        draft = v;
        localError = '';
      }
    });
  });

  let modified = $derived(!isDefault(spec, value));
  let numeric = $derived(spec.type === 'int' || spec.type === 'float');
  let named = $derived(specialLabel(spec, value));
  let usually = $derived(typicalText(spec));
  let help = $derived(helpParts(spec.help ?? ''));
  // The plan's value, offered only when it is one this field takes: a decision can also be a word
  // ("cpu only") that is a reason, not a value to type.
  let suggested = $derived.by(() => {
    const s = planSuggestion(value, decision?.value);
    return s !== undefined && coerceInput(spec, s).ok ? s : undefined;
  });

  function commit(raw: string | boolean) {
    const r = coerceInput(spec, raw);
    if (!r.ok) {
      localError = r.error;
      return;
    }
    localError = '';
    settings.set(spec.key, r.value);
  }

  function onText(e: Event & { currentTarget: HTMLInputElement }) {
    draft = e.currentTarget.value;
    commit(draft);
  }

  function onBlur() {
    focused = false;
    settings.flush();
  }

</script>

<div class="field" class:modified>
  <div class="head">
    {#if spec.type === 'bool'}
      <label class="toggle">
        <input
          id={id}
          type="checkbox"
          role="switch"
          checked={value === true}
          onchange={(e) => commit(e.currentTarget.checked)}
        />
        <span class="label">{spec.label}</span>
      </label>
    {:else}
      <label class="label" for={id}>{spec.label}</label>
    {/if}
    <span class="badges">
      {#if spec.lossy}<span class="badge warn" title="This setting changes what the model outputs">changes output</span>{/if}
      {#if spec.level === 'basic'}<span class="badge ok">basic</span>{/if}
      {#if spec.level === 'advanced'}<span class="badge">advanced</span>{/if}
      {#if spec.level === 'experimental'}<span class="badge warn">experimental</span>{/if}
      {#if spec.level === 'debug'}<span class="badge">debug</span>{/if}
      {#if spec.scope === 'request'}
        <span class="badge info">applies to next message</span>
      {:else if spec.scope === 'session'}
        <span class="badge">needs reload</span>
      {/if}
      {#if userSet}<span class="badge info" title="You set this value; defaults and plans do not change it">set by you</span>{/if}
      {#if modified}<span class="badge" title="Default: {displayValue(spec.default)}">modified</span>{/if}
      {#if decision}<SourceBadge source={decision.source} reason={decision.reason} />{/if}
    </span>
  </div>

  {#if spec.type !== 'bool'}
    <div class="control">
      {#if spec.type === 'choice' && spec.choices}
        <select id={id} value={displayValue(value)} onchange={(e) => commit(e.currentTarget.value)}>
          {#each spec.choices as c (c.value)}
            <option value={c.value}>{c.label}</option>
          {/each}
        </select>
      {:else if numeric && !spec.accepts_auto}
        <input
          id={id}
          type="number"
          min={spec.min}
          max={spec.max}
          step={spec.type === 'int' ? 1 : 'any'}
          value={draft}
          oninput={onText}
          onfocus={() => (focused = true)}
          onblur={onBlur}
        />
      {:else}
        <input
          id={id}
          type="text"
          inputmode={numeric ? 'decimal' : undefined}
          placeholder={numeric ? 'number or auto' : ''}
          value={draft}
          oninput={onText}
          onfocus={() => (focused = true)}
          onblur={onBlur}
          class:wide={spec.type === 'path' || spec.type === 'text'}
        />
        {#if numeric && spec.accepts_auto && draft.trim().toLowerCase() !== 'auto'}
          <button type="button" onclick={() => { draft = 'auto'; commit('auto'); settings.flush(); }}>Auto</button>
        {/if}
      {/if}
      {#if spec.unit && numeric}<span class="muted unit">{spec.unit}</span>{/if}
      {#if named}<span class="named">= {named}</span>{/if}
      {#if usually}<span class="muted range">{usually}</span>{/if}
      {#if suggested !== undefined}
        <span class="muted range">plan suggests {suggested}{spec.unit ? ` ${spec.unit}` : ''}</span>
        <button type="button" class="link" onclick={() => { commit(suggested ?? ''); settings.flush(); }}>Use</button>
      {/if}
      {#if userSet || modified}
        <button type="button" class="link" onclick={() => settings.reset(spec.key)}>Reset to default</button>
      {/if}
    </div>
  {:else if userSet || modified}
    <div class="control">
      <button type="button" class="link" onclick={() => settings.reset(spec.key)}>Reset to default</button>
    </div>
  {/if}

  {#if localError || rejected}
    <p class="err" role="alert">{localError || rejected}</p>
  {/if}
  {#if help.lead}
    <p class="lead muted">{help.lead}</p>
  {/if}
  <details class="help">
    <summary>More <span class="visually-hidden">about {spec.label}</span></summary>
    {#if help.rest}<p>{help.rest}</p>{/if}
    {#if spec.special?.length}
      <p>{spec.special.map((s) => `${displayValue(s.value)} = ${s.label}`).join('; ')}</p>
    {/if}
    <p class="muted flag"><code>{spec.flag || (spec.switches ?? []).map((s) => s.flag).join(' ')}</code> default <code>{displayValue(spec.default)}</code>{#if spec.min !== undefined && numeric}, accepted from {spec.min}{spec.max !== undefined ? ` to ${spec.max}` : ''}{/if}</p>
  </details>
</div>

<style>
  .field {
    padding: 0.65rem 0;
    border-bottom: 1px solid var(--border);
    border-left: 3px solid transparent;
    padding-left: 0.6rem;
  }
  .field.modified {
    border-left-color: var(--accent);
  }
  .head {
    display: flex;
    align-items: center;
    gap: 0.5rem 0.75rem;
    flex-wrap: wrap;
  }
  .label {
    font-weight: 600;
  }
  .toggle {
    display: inline-flex;
    align-items: center;
    gap: 0.5rem;
    cursor: pointer;
  }
  .toggle input {
    width: 1.1rem;
    height: 1.1rem;
    accent-color: var(--accent);
  }
  .badges {
    display: inline-flex;
    gap: 0.3rem;
    flex-wrap: wrap;
  }
  .control {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    flex-wrap: wrap;
    margin-top: 0.35rem;
  }
  .control input[type='number'],
  .control input[type='text'] {
    width: 12rem;
  }
  .control input.wide {
    width: 100%;
    max-width: 40rem;
  }
  .range,
  .unit {
    font-size: 0.8rem;
  }
  .named {
    font-size: 0.85rem;
    font-weight: 600;
  }
  .lead {
    margin: 0.3rem 0 0;
    font-size: 0.85rem;
  }
  .err {
    color: var(--err);
    font-size: 0.85rem;
    margin: 0.3rem 0 0;
  }
  .help {
    margin-top: 0.3rem;
    font-size: 0.85rem;
  }
  .help summary {
    cursor: pointer;
    color: var(--muted);
  }
  .help p {
    margin: 0.3rem 0;
    white-space: pre-line;
  }
  .flag {
    font-size: 0.8rem;
  }
</style>
