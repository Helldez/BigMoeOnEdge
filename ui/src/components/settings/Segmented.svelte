<script lang="ts" generics="T extends string | number">
  // A row of choices where exactly one holds: radio semantics, arrow keys move between them.
  let { options, value, label, onchange }: { options: { value: T; label: string }[]; value: T; label: string; onchange: (v: T) => void } = $props();

  function key(e: KeyboardEvent, i: number) {
    const d = e.key === 'ArrowRight' || e.key === 'ArrowDown' ? 1 : e.key === 'ArrowLeft' || e.key === 'ArrowUp' ? -1 : 0;
    if (!d) return;
    e.preventDefault();
    const next = options[(i + d + options.length) % options.length];
    onchange(next.value);
    const group = (e.currentTarget as HTMLElement).parentElement;
    queueMicrotask(() => group?.querySelector<HTMLElement>('[aria-checked="true"]')?.focus());
  }
</script>

<div class="seg" role="radiogroup" aria-label={label}>
  {#each options as o, i (o.value)}
    <button type="button" role="radio" aria-checked={o.value === value} tabindex={o.value === value ? 0 : -1} onclick={() => onchange(o.value)} onkeydown={(e) => key(e, i)}>{o.label}</button>
  {/each}
</div>

<style>
  .seg {
    display: inline-flex;
    flex-wrap: wrap;
    gap: 2px;
    padding: 2px;
    border: var(--edge) solid var(--line);
    border-radius: var(--radius);
    background: var(--surface-2);
  }
  button {
    min-height: 2rem;
    border: none;
    box-shadow: none;
    background: transparent;
    font-size: 0.875rem;
    border-radius: var(--radius-sm);
    color: var(--muted);
  }
  button[aria-checked='true'] {
    background: var(--surface);
    color: var(--text);
    box-shadow: var(--shadow-sm);
  }
</style>
