<!--
  Where models are kept: the folder downloads land in, and other folders whose models are listed and
  loaded in place. The server decides what it takes (docs/server-api.md, "Models"): an absolute path,
  a models folder it can write to, scan folders that exist; it refuses a change from another machine
  and while a download runs, and says why. Folders the server was started with stay as they are.
  In the desktop app a folder is picked with the system's dialog; in a browser it is typed.
-->
<script lang="ts">
  import { api } from '../../lib/api';
  import { desktop, isDesktop } from '../../lib/desktop';
  import type { ModelsResponse } from '../../lib/types';

  let { data, onChanged }: { data: ModelsResponse; onChanged: (next: ModelsResponse) => void } = $props();

  let folders = $derived(data.folders);
  let typing = $state<'' | 'models' | 'scan'>('');
  let typed = $state('');
  let busy = $state(false);
  let error = $state('');

  async function save(change: { models_dir?: string; scan_dirs?: string[] }) {
    busy = true;
    error = '';
    try {
      onChanged(await api.setModelFolders(change));
      typing = '';
      typed = '';
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
    } finally {
      busy = false;
    }
  }

  async function choose(which: 'models' | 'scan') {
    if (!isDesktop()) {
      typing = which;
      typed = '';
      return;
    }
    const path = await desktop.pickFolder();
    if (path) await apply(which, path);
  }

  function apply(which: 'models' | 'scan', path: string) {
    if (which === 'models') return save({ models_dir: path });
    return save({ scan_dirs: [...(folders?.chosen_scan_dirs ?? []), path] });
  }

  function removeScan(path: string) {
    return save({ scan_dirs: (folders?.chosen_scan_dirs ?? []).filter((d) => d !== path) });
  }

  function submitTyped(e: SubmitEvent) {
    e.preventDefault();
    if (typing && typed.trim()) apply(typing, typed.trim());
  }
</script>

<div class="folders">
  <p class="muted">
    Downloads land in <code>{data.models_dir}</code>; a gguf you copy there (or into a folder directly inside it) is listed too.
  </p>
  {#if folders && !folders.models_dir_at_launch}
    <div class="actions">
      <button type="button" disabled={busy} onclick={() => choose('models')}>Change folder</button>
      {#if folders.chosen_models_dir}
        <button type="button" disabled={busy} onclick={() => save({ models_dir: '' })}>Use the default folder</button>
      {/if}
    </div>
    <p class="muted hint">
      Models already downloaded stay where they are: move them to the new folder, or add the old one below so they are still listed.
    </p>
  {:else if folders}
    <p class="muted hint">Set when the server was started (<code>--models-dir</code>).</p>
  {/if}

  {#if folders}
    <h4>Other folders</h4>
    <p class="muted hint">Models in these folders are listed and loaded in place; nothing is written there.</p>
    <ul class="list">
      {#each folders.launch_scan_dirs as d (d)}
        <li><code>{d}</code> <span class="muted">set when the server was started</span></li>
      {/each}
      {#each folders.chosen_scan_dirs as d (d)}
        <li>
          <code>{d}</code>
          <button type="button" class="link" disabled={busy} onclick={() => removeScan(d)} aria-label={`Stop listing ${d}`}>Remove</button>
        </li>
      {/each}
    </ul>
    <button type="button" disabled={busy} onclick={() => choose('scan')}>Add a folder</button>

    {#if typing}
      <form class="row" onsubmit={submitTyped}>
        <input
          bind:value={typed}
          placeholder={typing === 'models' ? 'Full path of the new models folder' : 'Full path of a folder with models'}
          aria-label={typing === 'models' ? 'Full path of the new models folder' : 'Full path of a folder with models'}
          autocomplete="off"
        />
        <button type="submit" class="primary" disabled={!typed.trim() || busy}>{typing === 'models' ? 'Use this folder' : 'Add'}</button>
        <button type="button" onclick={() => (typing = '')}>Cancel</button>
      </form>
    {/if}
  {:else if data.scan_dirs?.length}
    <p class="muted">
      Also listed, and loaded in place: {#each data.scan_dirs as d, i (d)}{i ? ', ' : ''}<code>{d}</code>{/each}.
    </p>
  {/if}
  {#if error}<p class="banner error" role="alert">{error}</p>{/if}
</div>

<style>
  .folders {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
    align-items: flex-start;
  }
  .folders p {
    margin: 0;
  }
  .actions,
  .row {
    display: flex;
    gap: 0.5rem;
    flex-wrap: wrap;
    align-items: center;
  }
  .row {
    align-self: stretch;
  }
  .row input {
    flex: 1 1 16rem;
    min-width: 0;
  }
  .hint {
    font-size: 0.85rem;
  }
  h4 {
    margin: 0.5rem 0 0;
  }
  .list {
    list-style: none;
    padding: 0;
    margin: 0;
    display: flex;
    flex-direction: column;
    gap: 0.25rem;
  }
  .list li {
    display: flex;
    gap: 0.5rem;
    flex-wrap: wrap;
    align-items: baseline;
    overflow-wrap: anywhere;
  }
</style>
