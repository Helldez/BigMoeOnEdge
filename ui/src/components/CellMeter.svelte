<script lang="ts">
  // The project's meter: a row of square cells, the logo's pixel grid turned into a gauge
  // (docs/design.md, "The signature"). A share is counted in cells, the way the engine counts
  // experts in a cache, rather than drawn as a smooth pill; an optional mark outlines the cell of a
  // reference value (what the plan credited). Drawn with classes only, no style attribute, so the
  // server's Content-Security-Policy has nothing to allow.
  let {
    value,
    mark = null,
    cells = 20,
    label,
    valueText = '',
    small = false,
  }: { value: number; mark?: number | null; cells?: number; label: string; valueText?: string; small?: boolean } = $props();

  const clamp = (v: number) => Math.min(100, Math.max(0, v));
  const filled = $derived(Math.round((clamp(value) / 100) * cells));
  // The cell the reference value falls in, or -1 without one.
  const markAt = $derived(mark === null ? -1 : Math.min(cells - 1, Math.max(0, Math.ceil((clamp(mark) / 100) * cells) - 1)));
</script>

<span
  class="cells"
  class:small
  role="meter"
  aria-label={label}
  aria-valuemin={0}
  aria-valuemax={100}
  aria-valuenow={Math.round(clamp(value))}
  aria-valuetext={valueText || undefined}
>
  {#each { length: cells } as _, i (i)}
    <i class:on={i < filled} class:mark={i === markAt}></i>
  {/each}
</span>

<style>
  .cells {
    display: inline-grid;
    grid-auto-flow: column;
    grid-auto-columns: 1fr;
    gap: 2px;
    width: 100%;
    height: 8px;
    vertical-align: middle;
  }
  .cells.small {
    width: 5rem;
    height: 6px;
    gap: 1px;
  }
  i {
    display: block;
    background: var(--surface-2);
    border-radius: 1px;
  }
  i.on {
    background: var(--accent);
  }
  /* The reference cell: an outline in the words' colour, seen on an empty cell and a full one. */
  i.mark {
    outline: 2px solid var(--text);
    outline-offset: 1px;
  }
</style>
