<script lang="ts">
  // A unified diff, line by line: what an edit removed and added, with the sign kept in the text so
  // the change reads without its colour (docs/design.md, "Contrast").
  let { diff }: { diff: string } = $props();
  let lines = $derived(
    diff
      .replace(/\n$/, '')
      .split('\n')
      .map((text) => ({
        text,
        kind: text.startsWith('+++') || text.startsWith('---') ? 'head' : text.startsWith('@@') ? 'hunk' : text.startsWith('+') ? 'add' : text.startsWith('-') ? 'del' : 'ctx',
      })),
  );
</script>

<pre class="diff" aria-label="Diff">{#each lines as l, i (i)}<span class="line {l.kind}">{l.text || ' '}</span>{/each}</pre>

<style>
  .diff {
    margin: 0;
    padding: var(--s2) 0;
    font-size: 0.8125rem;
    line-height: 1.5;
    max-height: 24rem;
    overflow: auto;
  }
  .line {
    display: block;
    padding: 0 var(--s3);
    white-space: pre;
  }
  .add {
    background: var(--ok-tint);
    color: var(--text);
  }
  .del {
    background: var(--err-tint);
    color: var(--text);
  }
  .hunk {
    color: var(--accent-ink);
  }
  .head {
    color: var(--muted);
  }
</style>
