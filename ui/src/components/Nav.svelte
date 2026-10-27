<script lang="ts">
  import { href, type Route } from '../lib/route';
  import { router } from '../stores/router.svelte';

  // The five screens, in the order every UI of this project shows them (docs/screens.md).
  const items: { route: Route; label: string }[] = [
    { route: 'chat', label: 'Chat' },
    { route: 'models', label: 'Models' },
    { route: 'plan', label: 'Plan' },
    { route: 'settings', label: 'Settings' },
    { route: 'metrics', label: 'Metrics' },
  ];
</script>

<nav aria-label="Main">
  {#each items as item (item.route)}
    <a href={href(item.route)} aria-current={router.route === item.route ? 'page' : undefined}>{item.label}</a>
  {/each}
</nav>

<style>
  nav {
    display: flex;
    gap: 0.25rem;
    flex-wrap: wrap;
  }
  a {
    padding: 0.3rem 0.75rem;
    border: var(--edge) solid transparent;
    text-decoration: none;
    color: var(--muted);
    font-size: 1rem;
  }
  a:hover {
    color: var(--text);
    border-color: var(--line);
  }
  a[aria-current='page'] {
    color: var(--on-accent);
    background: var(--accent);
    border-color: var(--shadow);
    box-shadow: var(--shadow-sm);
    font-weight: 700;
  }
</style>
