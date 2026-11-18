<script lang="ts" module>
  export interface MenuItem {
    value: string;
    label: string;
    description?: string;
    tone?: '' | 'warn' | 'err';
    /** Shown, not pickable: its description says why. */
    disabled?: boolean;
    /** In a menu of switches (`switches`), whether this one is on. */
    on?: boolean;
  }
</script>

<script lang="ts">
  // A small popover menu: a button that opens a list of choices, each with a line saying what it
  // does. Arrow keys move, Enter picks, Escape closes and gives focus back to the button.
  import type { Snippet } from 'svelte';
  import Icon from './Icon.svelte';

  let {
    items,
    value = '',
    label,
    onselect,
    trigger,
    footer,
    up = true,
    switches = false,
    chevron = true,
  }: {
    items: MenuItem[];
    value?: string;
    label: string;
    onselect: (value: string) => void;
    trigger: Snippet;
    footer?: Snippet<[() => void]>;
    up?: boolean;
    /** Items that each turn something on or off, or act, instead of one choice among them. */
    switches?: boolean;
    chevron?: boolean;
  } = $props();

  let open = $state(false);
  let root: HTMLElement | undefined = $state();
  let button: HTMLButtonElement | undefined = $state();
  const id = `menu-${Math.random().toString(36).slice(2, 8)}`;

  function close(refocus = true) {
    open = false;
    if (refocus) button?.focus();
  }
  function pick(v: string) {
    onselect(v);
    close();
  }
  function focusItem(delta: number) {
    const els = [...(root?.querySelectorAll<HTMLElement>('.item:not(:disabled), .menu-foot button') ?? [])];
    if (els.length === 0) return;
    const i = els.indexOf(document.activeElement as HTMLElement);
    els[(i + delta + els.length) % els.length].focus();
  }
  function onkey(e: KeyboardEvent) {
    if (!open) return;
    if (e.key === 'Escape') {
      e.preventDefault();
      close();
    } else if (e.key === 'ArrowDown') {
      e.preventDefault();
      focusItem(1);
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      focusItem(-1);
    }
  }
  $effect(() => {
    if (!open) return;
    const away = (e: PointerEvent) => {
      if (root && !root.contains(e.target as Node)) close(false);
    };
    document.addEventListener('pointerdown', away);
    queueMicrotask(() => focusItem(1));
    return () => document.removeEventListener('pointerdown', away);
  });
</script>

<!-- svelte-ignore a11y_no_static_element_interactions -->
<div class="menu-root" bind:this={root} onkeydown={onkey}>
  <button
    type="button"
    class="chip"
    bind:this={button}
    aria-haspopup="menu"
    aria-expanded={open}
    aria-controls={id}
    title={label}
    onclick={() => (open = !open)}
  >
    {@render trigger()}
    {#if chevron}<span class="chev"><Icon name="chevronDown" size={14} /></span>{/if}
  </button>
  {#if open}
    <div class="pop" class:up id={id} role="menu" aria-label={label}>
      {#each items as it (it.value)}
        {@const checked = switches ? !!it.on : it.value === value}
        <button
          type="button"
          role={switches ? 'menuitemcheckbox' : 'menuitemradio'}
          aria-checked={checked}
          class="item {it.tone ?? ''}"
          disabled={it.disabled}
          onclick={() => pick(it.value)}
        >
          <span class="check">{#if checked}<Icon name="check" size={16} />{/if}</span>
          <span class="words">
            <span class="label">{it.label}</span>
            {#if it.description}<span class="desc">{it.description}</span>{/if}
          </span>
        </button>
      {/each}
      {#if footer}<div class="menu-foot">{@render footer(() => close(false))}</div>{/if}
    </div>
  {/if}
</div>

<style>
  .menu-root {
    position: relative;
    display: inline-flex;
  }
  .chip {
    min-height: 2rem;
    padding: 0 var(--s2);
    gap: var(--s1);
    border-radius: var(--pill);
    font-size: 0.8125rem;
    font-weight: 550;
    box-shadow: none;
    max-width: 13rem;
  }
  @media (max-width: 640px) {
    .chip {
      max-width: 9.5rem;
    }
  }
  .chev {
    display: inline-flex;
  }
  /* Under a finger every control is a 44 px target (docs/design.md, "Geometry"). */
  @media (pointer: coarse), (max-width: 640px) {
    .chip {
      min-height: 44px;
      min-width: 44px;
      justify-content: center;
    }
  }
  /* On a phone's width a chip may give its words up for its icon (`.squeeze`, the caller's
     choice): the words stay for a screen reader, and the menu still names each choice. */
  @media (max-width: 640px) {
    .chev {
      display: none;
    }
    .chip :global(.squeeze) {
      position: absolute;
      width: 1px;
      height: 1px;
      overflow: hidden;
      clip: rect(0 0 0 0);
      white-space: nowrap;
    }
  }
  .chip :global(.chip-text) {
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .pop {
    position: absolute;
    left: 0;
    top: calc(100% + 4px);
    z-index: 30;
    min-width: 16rem;
    max-width: min(22rem, calc(100vw - 2rem));
    max-height: 22rem;
    overflow-y: auto;
    padding: var(--s1);
    background: var(--surface);
    border: var(--edge) solid var(--line);
    border-radius: var(--radius);
    box-shadow: var(--shadow-md);
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .pop.up {
    top: auto;
    bottom: calc(100% + 4px);
  }
  .item {
    display: flex;
    align-items: flex-start;
    justify-content: flex-start;
    gap: var(--s2);
    min-height: 2.25rem;
    padding: var(--s2);
    border: none;
    box-shadow: none;
    background: transparent;
    text-align: left;
    white-space: normal;
    font-weight: 500;
  }
  .item:hover:not(:disabled),
  .item:focus-visible {
    background: var(--hover);
  }
  .item:disabled {
    opacity: 1;
    cursor: not-allowed;
  }
  .item:disabled .label {
    color: var(--muted);
  }
  .check {
    width: 1rem;
    flex: none;
    color: var(--accent-ink);
  }
  .words {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .label {
    font-size: 0.875rem;
  }
  .desc {
    font-size: 0.75rem;
    color: var(--muted);
    font-weight: 400;
  }
  .item.warn .label {
    color: var(--warn);
  }
  .menu-foot {
    border-top: var(--edge) solid var(--line);
    margin-top: var(--s1);
    padding-top: var(--s1);
    display: flex;
    flex-direction: column;
  }
  .menu-foot :global(button) {
    justify-content: flex-start;
    border: none;
    box-shadow: none;
    background: transparent;
    font-weight: 500;
    font-size: 0.875rem;
  }
  .menu-foot :global(button:hover) {
    background: var(--hover);
  }
</style>
