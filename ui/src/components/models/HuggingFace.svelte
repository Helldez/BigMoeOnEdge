<!--
  A model from a Hugging Face repository, in three steps the server does: list what the
  repository holds, read one model's header against this machine, add it to the catalog. The
  download is then the catalog's (docs/server-api.md, "Models"). Nothing here reads a gguf or
  names an engine parameter: the facts card shows what the server answered.
-->
<script lang="ts">
  import { api, ApiError, CODE_NOT_A_REPO } from '../../lib/api';
  import { formatBytes, formatNum } from '../../lib/format';
  import type { HfFacts, HfModel, HfRepoHit } from '../../lib/types';

  let { onAdded }: { onAdded: (id: string) => Promise<void> | void } = $props();

  let repo = $state('');
  let token = $state('');
  let askToken = $state(false);
  let listed = $state(''); // the repository the list below came from
  let models = $state<HfModel[] | null>(null);
  let hits = $state<HfRepoHit[] | null>(null); // a search's repositories, when the text was not one
  let selected = $state('');
  let facts = $state<HfFacts | null>(null);
  let busy = $state('');
  let error = $state('');
  let added = $state('');

  let chosen = $derived(models?.find((m) => m.name === selected) ?? null);

  async function run(what: string, fn: () => Promise<void>) {
    busy = what;
    error = '';
    try {
      await fn();
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
    } finally {
      busy = '';
    }
  }

  function list() {
    run('list', async () => {
      facts = null;
      added = '';
      hits = null;
      let r;
      try {
        r = await api.hfModels(repo, token || undefined);
      } catch (e) {
        // Not a repository: a name to search for, and the repositories found are offered instead.
        if (!(e instanceof ApiError) || e.code !== CODE_NOT_A_REPO) throw e;
        models = null;
        hits = (await api.hfSearch(repo, token || undefined)).repos;
        return;
      }
      models = r.models;
      listed = r.repo;
      // A URL that named a file preselects it; a repository with one model needs no choice.
      selected = r.file && r.models.some((m) => m.name === r.file) ? r.file : r.models.length === 1 ? r.models[0].name : '';
    });
  }

  function inspect() {
    run('inspect', async () => {
      facts = await api.hfInspect(listed, selected, token || undefined);
    });
  }

  function add() {
    run('add', async () => {
      const r = await api.hfAdd(listed, selected, token || undefined);
      added = r.id;
      await onAdded(r.id);
    });
  }

  function pick(hit: HfRepoHit) {
    repo = hit.id;
    list();
  }

  function choose(name: string) {
    selected = name;
    facts = null;
    added = '';
  }

  const modeLabel: Record<string, string> = { streams: 'streams', 'plain mmap': 'plain mmap', 'not readable': 'not readable' };
</script>

<section class="card hf">
  <h3>From Hugging Face</h3>
  <p class="muted">A model's name to search for, or a repository id or URL. The server lists the repository's gguf files, reads a model's header before any download, and says what it is and whether it fits here.</p>
  <form
    class="row"
    onsubmit={(e) => {
      e.preventDefault();
      list();
    }}
  >
    <input type="text" bind:value={repo} placeholder="a name to search, owner/name, or a huggingface.co link" aria-label="Repository or search" autocomplete="off" />
    <button type="submit" class="primary" disabled={!repo.trim() || busy !== ''}>{busy === 'list' ? 'Looking...' : 'Find'}</button>
    <button type="button" onclick={() => (askToken = !askToken)}>{askToken ? 'Hide token' : 'Token'}</button>
  </form>
  {#if askToken}
    <label class="row token">
      <span class="muted">Access token for a gated or private repository. Used for these requests and kept nowhere.</span>
      <input type="password" bind:value={token} autocomplete="off" aria-label="Hugging Face access token" />
    </label>
  {/if}
  {#if error}<p class="banner error" role="alert">{error}</p>{/if}

  {#if hits}
    {#if hits.length === 0}
      <p class="muted">No repository with gguf files matches <code>{repo}</code>.</p>
    {:else}
      <ul class="models" aria-label="Repositories found">
        {#each hits as h (h.id)}
          <li>
            <button type="button" class="link" onclick={() => pick(h)}>{h.id}</button>
            <span class="muted">{formatNum(h.downloads, 0)} downloads · {formatNum(h.likes, 0)} likes</span>
            {#if h.gated}<span class="badge warn" title="Needs an access token">gated</span>{/if}
          </li>
        {/each}
      </ul>
    {/if}
  {/if}

  {#if models}
    {#if models.length === 0}
      <p class="muted">No gguf files in <code>{listed}</code>.</p>
    {:else}
      <ul class="models" aria-label="Models in {listed}">
        {#each models as m (m.name)}
          <li class:chosen={m.name === selected}>
            <label>
              <input type="radio" name="hf-model" value={m.name} checked={m.name === selected} onchange={() => choose(m.name)} />
              <span class="name">{m.name}</span>
              {#if m.quant}<span class="badge">{m.quant}</span>{/if}
              <span class="muted">{formatBytes(m.bytes)}</span>
              {#if m.count > 1}<span class="muted">{m.files.length} of {m.count} shards</span>{/if}
              {#if !m.complete}<span class="badge err">incomplete</span>{/if}
            </label>
          </li>
        {/each}
      </ul>
      <div class="actions">
        <button type="button" class="primary" disabled={!chosen || busy !== ''} onclick={inspect}>
          {busy === 'inspect' ? 'Reading the header...' : 'Check'}<span class="visually-hidden"> {selected}</span>
        </button>
      </div>
    {/if}
  {/if}

  {#if facts && chosen}
    {@const f = facts}
    <div class="facts">
      <header>
        <strong>{f.name}</strong>
        <span class="badge {f.verdict.mode === 'streams' ? 'ok' : f.verdict.mode === 'not readable' ? 'err' : 'warn'}">
          {modeLabel[f.verdict.mode] ?? f.verdict.mode}
        </span>
      </header>
      {#if f.model.readable}
        <dl>
          <dt>Architecture</dt>
          <dd>{f.model.arch || '?'}</dd>
          <dt>Experts</dt>
          <dd>{f.model.is_moe ? `${f.model.n_expert}, ${f.model.n_expert_used} per token` : 'none (dense)'}</dd>
          {#if f.model.streamable}
            <dt>Layers</dt>
            <dd>{f.model.n_layer} ({f.model.n_moe_layer} with experts)</dd>
            <dt>Resident</dt>
            <dd>{formatBytes(f.model.dense_bytes)}</dd>
            <dt>Experts on flash</dt>
            <dd>{formatBytes(f.model.expert_bytes)}, {formatBytes(f.model.token_cycle_bytes)} a token</dd>
          {/if}
          <dt>On disk</dt>
          <dd>{formatBytes(f.bytes)}{f.files > 1 ? ` in ${f.files} files` : ''}</dd>
        </dl>
      {/if}
      <p class="muted">{f.verdict.memory}</p>
      <p class={f.verdict.downloadable ? 'muted' : 'err'}>{f.verdict.disk}</p>
      <div class="actions">
        {#if added}
          <span class="badge ok">added to the catalog</span>
        {:else}
          <button type="button" class="primary" disabled={!f.verdict.downloadable || !chosen.complete || busy !== ''} onclick={add}>
            {busy === 'add' ? 'Adding...' : 'Add to the catalog'}<span class="visually-hidden"> {f.name}</span>
          </button>
        {/if}
      </div>
    </div>
  {/if}
</section>

<style>
  .hf h3 {
    margin: 0 0 0.3rem;
    font-size: 1rem;
  }
  .row {
    display: flex;
    gap: 0.5rem;
    flex-wrap: wrap;
    align-items: center;
  }
  .row input[type='text'],
  .row input[type='password'] {
    flex: 1 1 16rem;
    min-width: 0;
  }
  .token {
    margin-top: 0.5rem;
    flex-direction: column;
    align-items: stretch;
  }
  .models {
    list-style: none;
    padding: 0;
    margin: 0.75rem 0 0;
    display: flex;
    flex-direction: column;
    gap: 0.25rem;
  }
  .models li {
    display: flex;
    gap: 0.5rem;
    align-items: center;
    flex-wrap: wrap;
  }
  .models label {
    display: flex;
    gap: 0.5rem;
    align-items: center;
    flex-wrap: wrap;
    cursor: pointer;
  }
  .models .name {
    overflow-wrap: anywhere;
  }
  .models li.chosen {
    background: var(--ok-bg);
  }
  .facts {
    margin-top: 0.75rem;
    padding-top: 0.5rem;
    border-top: 2px solid var(--border);
  }
  .facts header {
    display: flex;
    gap: 0.5rem;
    align-items: center;
    flex-wrap: wrap;
  }
  .facts dl {
    display: grid;
    grid-template-columns: max-content 1fr;
    gap: 0.2rem 0.75rem;
    margin: 0.5rem 0;
  }
  .facts dt {
    color: var(--muted);
  }
  .facts dd {
    margin: 0;
  }
  .facts p {
    margin: 0.3rem 0;
  }
  .err {
    color: var(--err);
  }
  .actions {
    margin-top: 0.5rem;
    display: flex;
    gap: 0.5rem;
    align-items: center;
  }
</style>
