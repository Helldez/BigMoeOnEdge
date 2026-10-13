<script lang="ts">
  import { OPTIONAL_LEVELS } from '../../lib/schema';
  import { settings } from '../../stores/settings.svelte';

  let { counts }: { counts: Record<string, number> } = $props();
  const title = (s: string) => s.charAt(0).toUpperCase() + s.slice(1);
</script>

<fieldset class="levels">
  <legend>Show</legend>
  <label><input type="checkbox" checked disabled /> Basic ({counts.basic ?? 0})</label>
  {#each OPTIONAL_LEVELS as level (level)}
    <label>
      <input
        type="checkbox"
        checked={settings.levels.includes(level)}
        onchange={(e) => settings.toggleLevel(level, e.currentTarget.checked)}
      />
      {title(level)} ({counts[level] ?? 0})
    </label>
  {/each}
</fieldset>

<style>
  .levels {
    display: flex;
    gap: 1rem;
    flex-wrap: wrap;
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 0.4rem 0.8rem 0.6rem;
    margin: 0 0 1rem;
  }
  legend {
    font-size: 0.8rem;
    color: var(--muted);
    padding: 0 0.3rem;
  }
  label {
    display: inline-flex;
    gap: 0.35rem;
    align-items: center;
  }
</style>
