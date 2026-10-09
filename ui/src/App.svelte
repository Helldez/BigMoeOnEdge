<script lang="ts">
  import { onMount } from 'svelte';
  import Nav from './components/Nav.svelte';
  import StatusBar from './components/StatusBar.svelte';
  import Chat from './routes/Chat.svelte';
  import Models from './routes/Models.svelte';
  import Settings from './routes/Settings.svelte';
  import Plan from './routes/Plan.svelte';
  import { router } from './stores/router.svelte';
  import { server } from './stores/server.svelte';

  onMount(() => {
    router.start();
    server.start();
  });
</script>

<div class="shell">
  <header class="top">
    <span class="brand">BigMoeOnEdge</span>
    <Nav />
  </header>
  <StatusBar />
  <main class="page">
    {#if router.route === 'chat'}
      <Chat />
    {:else if router.route === 'models'}
      <Models />
    {:else if router.route === 'settings'}
      <Settings />
    {:else}
      <Plan />
    {/if}
  </main>
</div>

<style>
  .shell {
    display: flex;
    flex-direction: column;
    min-height: 100vh;
  }
  .top {
    display: flex;
    align-items: center;
    gap: 1.5rem;
    flex-wrap: wrap;
    padding: 0.5rem 1rem;
    border-bottom: 1px solid var(--border);
    background: var(--surface);
  }
  .brand {
    font-weight: 700;
    letter-spacing: 0.01em;
  }
  .page {
    flex: 1;
    width: 100%;
    max-width: 1400px;
    margin: 0 auto;
    padding: 1rem;
    box-sizing: border-box;
    min-width: 0;
  }
</style>
