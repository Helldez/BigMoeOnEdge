<script lang="ts">
  import { href, type Route } from '../lib/route';
  import { router } from '../stores/router.svelte';

  const items: { route: Route; label: string }[] = [
    { route: 'chat', label: 'Chat' },
    { route: 'models', label: 'Models' },
    { route: 'settings', label: 'Settings' },
    { route: 'plan', label: 'Plan' },
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
    border-radius: var(--radius);
    text-decoration: none;
    color: var(--muted);
  }
  a:hover {
    color: var(--text);
    background: var(--surface-2);
  }
  a[aria-current='page'] {
    color: var(--text);
    background: var(--surface-2);
    font-weight: 600;
  }
</style>
