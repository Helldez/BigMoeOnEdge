<script lang="ts">
  import { onMount } from 'svelte';
  import Nav from './components/Nav.svelte';
  import StatusBar from './components/StatusBar.svelte';
  import Chat from './routes/Chat.svelte';
  import Metrics from './routes/Metrics.svelte';
  import Models from './routes/Models.svelte';
  import Settings from './routes/Settings.svelte';
  import Plan from './routes/Plan.svelte';
  import { chat } from './stores/chat.svelte';
  import { router } from './stores/router.svelte';
  import { server } from './stores/server.svelte';

  let apiKey = $state('');

  onMount(() => {
    router.start();
    server.start();
    chat.start();
  });

  function submitKey(e: SubmitEvent) {
    e.preventDefault();
    if (!apiKey.trim()) return;
    server.submitKey(apiKey);
    apiKey = '';
  }
</script>

<div class="shell">
  <header class="top">
    <a class="brand" href="#/chat"><img src="/logo.svg" alt="" width="24" height="24" /><span>BigMoeOnEdge</span></a>
    <Nav />
  </header>
  <StatusBar />
  {#if server.authRequired}
    <div class="veil" role="dialog" aria-modal="true" aria-labelledby="key-title">
      <form class="key" onsubmit={submitKey}>
        <h2 id="key-title">API key</h2>
        <p>This server was started with <code>--api-key</code>. Enter the key to use it from here.</p>
        <input type="password" bind:value={apiKey} placeholder="key" autocomplete="off" />
        <button type="submit" disabled={!apiKey.trim()}>Connect</button>
      </form>
    </div>
  {/if}
  <main class="page">
    {#if router.route === 'chat'}
      <Chat />
    {:else if router.route === 'models'}
      <Models />
    {:else if router.route === 'settings'}
      <Settings />
    {:else if router.route === 'metrics'}
      <Metrics />
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
    border-bottom: var(--edge) solid var(--border);
    background: var(--surface);
  }
  .brand {
    display: inline-flex;
    align-items: center;
    gap: 0.5rem;
    font-family: var(--pixel);
    font-weight: 700;
    font-size: 1.15rem;
    letter-spacing: 0.01em;
    color: var(--text);
    text-decoration: none;
  }
  .brand img {
    image-rendering: pixelated;
  }
  .veil {
    position: fixed;
    inset: 0;
    display: grid;
    place-items: center;
    background: rgb(0 0 0 / 0.45);
    z-index: 10;
  }
  .key {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
    width: min(24rem, calc(100vw - 2rem));
    padding: 1.25rem;
    border: var(--edge) solid var(--border);
    box-shadow: 4px 4px 0 var(--shadow);
    background: var(--surface);
  }
  .key h2 {
    margin: 0;
    font-size: 1.1rem;
  }
  .key p {
    margin: 0;
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
