<script lang="ts">
  // Model output as sanitised Markdown (lib/markdown), with a copy button on every code block. The
  // buttons are this UI's own, added after the sanitiser ran: the model's text can carry none.
  import { renderMarkdown } from '../../lib/markdown';
  import { t } from '../../stores/prefs.svelte';

  let { text }: { text: string } = $props();
  let html = $derived(renderMarkdown(text));

  function copyButtons(node: HTMLElement, _html: string) {
    const add = () => {
      for (const pre of node.querySelectorAll('pre')) {
        if (pre.parentElement?.classList.contains('code-block')) continue;
        const wrap = document.createElement('div');
        wrap.className = 'code-block';
        pre.replaceWith(wrap);
        wrap.appendChild(pre);
        const lang = pre.querySelector('code')?.className.match(/language-([\w+#.-]+)/)?.[1] ?? '';
        const bar = document.createElement('div');
        bar.className = 'code-bar';
        const name = document.createElement('span');
        name.textContent = lang;
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'ghost small';
        btn.textContent = t('action.copy');
        btn.addEventListener('click', async () => {
          try {
            await navigator.clipboard.writeText(pre.textContent ?? '');
            btn.textContent = t('action.copied');
          } catch {
            btn.textContent = t('action.copyFailed');
          }
          setTimeout(() => (btn.textContent = t('action.copy')), 1500);
        });
        bar.append(name, btn);
        wrap.prepend(bar);
      }
    };
    add();
    return { update: () => queueMicrotask(add) };
  }
</script>

<div class="md" use:copyButtons={html}>{@html html}</div>

<style>
  .md {
    overflow-wrap: anywhere;
    line-height: 1.6;
  }
  .md :global(p:first-child) {
    margin-top: 0;
  }
  .md :global(p:last-child) {
    margin-bottom: 0;
  }
  .md :global(p) {
    margin: 0 0 var(--s3);
  }
  .md :global(ul),
  .md :global(ol) {
    padding-left: 1.4rem;
    margin: 0 0 var(--s3);
  }
  .md :global(li) {
    margin: var(--s1) 0;
  }
  .md :global(h1),
  .md :global(h2),
  .md :global(h3) {
    font-size: 1.05rem;
    margin: var(--s4) 0 var(--s2);
  }
  .md :global(blockquote) {
    margin: 0 0 var(--s3);
    padding-left: var(--s3);
    border-left: 3px solid var(--line);
    color: var(--muted);
  }
  .md :global(table) {
    display: block;
    overflow-x: auto;
    margin: 0 0 var(--s3);
    font-size: 0.9rem;
  }
  .md :global(.code-block) {
    margin: 0 0 var(--s3);
    border: var(--edge) solid var(--line);
    border-radius: var(--radius);
    background: var(--code-bg);
    overflow: hidden;
  }
  .md :global(.code-block pre) {
    margin: 0;
    border: none;
    border-radius: 0;
    white-space: pre;
  }
  .md :global(.code-bar) {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 2px var(--s1) 2px var(--s3);
    border-bottom: var(--edge) solid var(--line);
    font-size: 0.75rem;
    color: var(--muted);
    font-family: var(--mono);
  }
  .md :global(.code-bar button) {
    font-family: var(--ui);
  }
  .md :global(img) {
    max-width: 100%;
  }
</style>
