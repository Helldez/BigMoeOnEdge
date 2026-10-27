<script lang="ts">
  // A modal dialog on the platform's own <dialog>: it traps focus, Escape closes it, and focus goes
  // back where it was. The caller owns `open`.
  import type { Snippet } from 'svelte';
  import Icon from './Icon.svelte';
  import { t } from '../stores/prefs.svelte';

  let {
    open = $bindable(false),
    title,
    wide = false,
    children,
    actions,
    onclose = () => {},
  }: { open?: boolean; title: string; wide?: boolean; children: Snippet; actions?: Snippet; onclose?: () => void } = $props();

  let el: HTMLDialogElement | undefined = $state();
  $effect(() => {
    if (!el) return;
    if (open && !el.open) el.showModal();
    else if (!open && el.open) el.close();
  });
</script>

<dialog
  bind:this={el}
  class:wide
  aria-labelledby="dialog-title"
  onclose={() => {
    open = false;
    onclose();
  }}
  onclick={(e) => {
    // A click on the backdrop (the dialog element itself, outside its box) closes it.
    if (e.target === el) open = false;
  }}
>
  {#if open}
    <div class="box">
      <header>
        <h2 id="dialog-title">{title}</h2>
        <button type="button" class="ghost icon" aria-label={t('action.close')} onclick={() => (open = false)}><Icon name="close" /></button>
      </header>
      <div class="content">{@render children()}</div>
      {#if actions}<footer>{@render actions()}</footer>{/if}
    </div>
  {/if}
</dialog>

<style>
  dialog {
    border: var(--edge) solid var(--line);
    border-radius: var(--radius);
    background: var(--surface);
    color: var(--text);
    box-shadow: var(--shadow-lg);
    padding: 0;
    width: min(32rem, calc(100vw - 2rem));
    max-height: calc(100vh - 2rem);
  }
  dialog.wide {
    width: min(48rem, calc(100vw - 2rem));
  }
  dialog::backdrop {
    background: var(--scrim);
  }
  .box {
    display: flex;
    flex-direction: column;
    max-height: calc(100vh - 2rem);
  }
  header {
    display: flex;
    align-items: center;
    gap: var(--s2);
    padding: var(--s3) var(--s3) var(--s2) var(--s5);
  }
  header h2 {
    flex: 1;
    margin: 0;
    font-size: 1.0625rem;
  }
  .content {
    padding: 0 var(--s5) var(--s4);
    overflow-y: auto;
  }
  footer {
    display: flex;
    justify-content: flex-end;
    gap: var(--s2);
    flex-wrap: wrap;
    padding: var(--s3) var(--s5);
    border-top: var(--edge) solid var(--line);
  }
</style>
