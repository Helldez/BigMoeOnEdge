// A stand-in for bmoe-server, for developing and looking at the UI without an engine or a model:
// `npm run dev:mock` (or `npx vite preview --mode mock` over a build). It answers the routes the UI calls with made-up
// data shaped as docs/server-api.md, docs/agent.md and docs/desktop-app.md say, keeps conversations
// in memory, and plays a short agent run (reasoning, tool calls, an approval, an answer) on every
// message. Nothing here is the server's behaviour: it is a fixture.
import type { Plugin, ViteDevServer, PreviewServer } from 'vite';

type Json = Record<string, unknown>;
// The response object the dev server hands a middleware, as far as this uses it.
interface Res {
  statusCode: number;
  setHeader(k: string, v: string): void;
  write(s: string): void;
  end(s?: string): void;
  on(ev: 'close', fn: () => void): void;
}
interface Req {
  method?: string;
  url?: string;
  on(ev: 'data', fn: (c: Uint8Array) => void): void;
  on(ev: 'end', fn: () => void): void;
}

const now = () => new Date().toISOString();
const ago = (min: number) => new Date(Date.now() - min * 60_000).toISOString();
let seq = 100;
const id = (p: string) => `${p}${++seq}`;

const MODEL = {
  path: '/home/me/models/Example-MoE-30B-A3B-Q4_K_M.gguf',
  name: 'Example-MoE-30B-A3B-Q4_K_M.gguf',
  arch: 'qwen3moe',
  n_ctx: 16384,
  n_expert_used: 8,
  think_ctl: 'template',
  load_s: 6.2,
  streaming: true,
  compute_threads: 8,
  busy_threads: 10,
  ctx_advice: { recommended: 16384, short: false, param: 'ctx-size', refused: false, warn: false },
};

const info = () => ({
  version: '0.39.0',
  overlap_available: true,
  planner_available: true,
  host: { ram_total_mib: 16384, ram_available_mib: 9100, cpu_threads: 10 },
  state: 'ready',
  error: '',
  model: MODEL,
  generating: false,
});

const workspaces = [{ id: 'w1', name: 'website', path: '/home/me/projects/website' }];

const DIFF = `--- a/src/style.css
+++ b/src/style.css
@@ -1,6 +1,9 @@
 body {
-  font-family: serif;
-  color: #333;
+  font-family: system-ui, sans-serif;
+  color: #1f2933;
+  line-height: 1.6;
 }
+
+.hero { padding: 4rem 1rem; }
 h1 { margin: 0; }`;

type Item = Json & { id: string; kind: string };
interface Conv {
  id: string;
  title: string;
  workspace: string | null;
  mode: string;
  route: string;
  state: string;
  updated: string;
  task: string | null;
  items: Item[];
}

const conversations: Conv[] = [
  {
    id: 'c1',
    title: 'Refresh the landing page',
    workspace: 'w1',
    mode: 'ask',
    route: 'local',
    state: 'waiting',
    updated: ago(3),
    task: null,
    items: [
      { id: 'i1', kind: 'user', text: 'Make the landing page look more modern, then check that the build still passes.' },
      {
        id: 'i2',
        kind: 'assistant',
        text: 'I will look at the page and its stylesheet first.',
        reasoning: 'The page is index.html with styles in src/style.css. Read both, then adjust typography and spacing.',
        streaming: false,
      },
      { id: 'i3', kind: 'tool', tool: 'read_file', title: 'Read index.html', args: { path: 'index.html' }, state: 'done', result: '<!doctype html>…' },
      { id: 'i4', kind: 'tool', tool: 'read_file', title: 'Read src/style.css', args: { path: 'src/style.css' }, state: 'done', result: 'body { font-family: serif; }' },
      { id: 'i5', kind: 'tool', tool: 'grep_search', title: 'Search for "hero"', args: { path: 'src', pattern: 'hero' }, state: 'done', result: 'index.ts:hero()\n\n---\nTotal matches: 1\n' },
      { id: 'i6', kind: 'tool', tool: 'edit_file', title: 'Edit src/style.css', args: { path: 'src/style.css' }, state: 'done', diff: DIFF, file: 'src/style.css' },
      { id: 'i7', kind: 'tool', tool: 'write_file', title: 'Write index.html', args: { path: 'index.html' }, state: 'done', file: 'index.html', diff: '--- a/index.html\n+++ b/index.html\n@@ -3 +3,2 @@\n-<h1>Hello</h1>\n+<section class="hero"><h1>Hello</h1>\n+<p>Fast, local, private.</p></section>' },
      {
        id: 'i8',
        kind: 'assistant',
        text: 'The page now uses a sans-serif face, darker text and more room around the hero. Here is the main change:\n\n```css\nbody {\n  font-family: system-ui, sans-serif;\n  line-height: 1.6; /* easier to read */\n}\n```\n\nNext I want to run the build to check nothing broke.',
        streaming: false,
      },
      { id: 'i9', kind: 'tool', tool: 'exec_shell_command', title: 'Run npm run build', args: { command: 'npm run build' }, state: 'waiting' },
    ],
  },
  {
    id: 'c2',
    title: 'What is a mixture of experts?',
    workspace: null,
    mode: 'ask',
    route: 'local',
    state: 'idle',
    updated: ago(60 * 26),
    task: null,
    items: [
      { id: 'j1', kind: 'user', text: 'What is a mixture of experts, in two sentences?' },
      {
        id: 'j2',
        kind: 'assistant',
        text: 'A **mixture of experts** is a model made of many smaller sub-networks ("experts") and a router that picks a few of them for each token. Only the picked experts run, so a very large model costs about as much per token as a small one.',
        streaming: false,
      },
    ],
  },
  {
    id: 'c3',
    title: 'Morning summary',
    workspace: 'w1',
    mode: 'read_only',
    route: 'local',
    state: 'idle',
    updated: ago(60 * 24 * 3),
    task: 't1',
    items: [
      { id: 'k1', kind: 'user', text: 'Summarise what changed in this folder since yesterday.' },
      { id: 'k2', kind: 'notice', level: 'info', text: 'This ran as a scheduled task: nothing was changed, and nothing waited for an approval.' },
      { id: 'k3', kind: 'assistant', text: 'Two files changed: `index.html` and `src/style.css`.', streaming: false },
    ],
  },
];

const skills = [
  { name: 'release-notes', title: 'Release notes', description: 'Write release notes from the changes since the last tag.', source: 'bundled', enabled: true, body: '1. List the commits…' },
  { name: 'code-review', title: 'Code review', description: 'Review a change for bugs and unclear names.', source: 'bundled', enabled: false, body: 'Read the diff…' },
  { name: 'my-style', title: 'My writing style', description: 'Short sentences, no jargon.', source: 'user', enabled: true, body: 'Write short sentences.' },
];

const tasks = [
  {
    id: 't1',
    title: 'Morning summary',
    prompt: 'Summarise what changed in this folder since yesterday.',
    workspace: 'w1',
    mode: 'read_only',
    route: 'local',
    schedule: { kind: 'weekly', at: '08:30', every_minutes: null, weekdays: [1, 2, 3, 4, 5] },
    enabled: true,
    last_run: { at: ago(60 * 24 * 3), conversation: 'c3', outcome: 'done' },
    next_run: new Date(Date.now() + 14 * 3600_000).toISOString(),
  },
];

const providers = [{ id: 'p1', name: 'Example Cloud', base_url: 'https://api.example.com/v1', models: ['example-large', 'example-small'], has_key: true }];

const connect = [
  { id: 'agent-cli', name: 'A coding agent (CLI)', how: 'Point its OpenAI-compatible endpoint at this server.', snippet: '{\n  "baseURL": "http://127.0.0.1:8765/v1",\n  "model": "local"\n}', file: '~/.config/agent/settings.json', writable: true },
  { id: 'editor', name: 'An editor extension', how: 'Paste these settings in the extension.', snippet: 'endpoint = http://127.0.0.1:8765/v1\nmodel = local', file: null, writable: false },
];

const PARAMS = [
  { key: 'ctx-size', label: 'Context size', type: 'int', group: 'context', group_label: 'Context', group_help: 'How much text the model holds at once.', group_order: 1, level: 'basic', scope: 'session', help: 'Tokens the model holds at once. More takes memory from the expert cache.', flag: '--ctx-size', default: 4096, min: 256, max: 262144, unit: 'tokens', typical: { min: 4096, max: 32768 } },
  { key: 'n-predict', label: 'Answer length limit', type: 'int', group: 'context', group_label: 'Context', group_order: 1, level: 'basic', scope: 'request', help: 'The most tokens one answer may have.', flag: '--n-predict', default: 2048, min: 1, max: 65536, unit: 'tokens' },
  { key: 'think', label: 'Reasoning', type: 'bool', group: 'context', group_label: 'Context', group_order: 1, level: 'basic', scope: 'request', help: 'Let a reasoning model think first.', flag: '--think', default: true },
  { key: 'cache-mib', label: 'Expert cache', type: 'int', group: 'memory', group_label: 'Memory', group_help: 'What stays in memory.', group_order: 2, level: 'advanced', scope: 'session', help: 'Memory kept for experts.', flag: '--cache-mib', default: 0, accepts_auto: true, unit: 'MiB', special: [{ value: 0, label: 'off' }] },
];

const CONFIG = { values: { 'ctx-size': 16384, 'n-predict': 2048, think: true, 'cache-mib': 'auto' }, user_keys: ['ctx-size'], launch_keys: [], args: ['--ctx-size', '16384'], valid: true, error: '', reload_required: false, reload_keys: [], plan: null, auto_plan: true };

const MODELS = {
  models_dir: '/home/me/models',
  scan_dirs: [],
  local: [{ name: MODEL.name, path: MODEL.path, dir: '/home/me/models', folder: '', role: 'model', files: [MODEL.name], bytes: 18_600_000_000, shards: 1, arch: 'qwen3moe', streamable: true, complete: true }],
  catalog: [
    { id: 'big', title: 'Example-MoE-35B', quant: 'Q4_K_M', file: 'Example-MoE-35B-Q4_K_M.gguf', bytes: 20_419_565_568, blurb: '3B active of 35B. Past the memory of most laptops.', status: 'available', note: '' },
    { id: 'small', title: 'Example-MoE-26B', quant: 'Q4_0', file: 'Example-MoE-26B-Q4_0.gguf', bytes: 14_618_145_824, blurb: '4B active of 26B. The gentlest start.', status: 'available', note: '', received: 5_100_000_000, partial: true },
  ],
};

const PLAN = {
  available: true,
  regime: 'stream',
  streaming_declined: false,
  decline_reason: '',
  decisions: [],
  values: {},
  args: ['--cache-mib', '7300'],
  explain: '',
  machine: '10 cores, 16 GiB, one integrated GPU',
  from_last_load: true,
  summary: { sentence: 'Experts stream from disk, with a 7.1 GiB cache', lanes: [{ phase: 'prompt', label: 'Prompt', on: ['GPU'] }, { phase: 'tokens', label: 'Tokens', on: ['CPU'] }] },
  predicted: { s_per_token: 0.072, cache_hit_pct: 71 },
  measured: { s_per_token: 0.066, cache_hit_pct: 74, tokens: 212 },
  devices: [
    { name: 'CPU', description: 'The processor', cpu: true, own_memory: false, memory_total: 0, prompt_ratio: 1, token_ratio: 1, uses: ['tokens'], status: 'used', badge: 'Tokens', why: 'Computes the tokens.' },
    { name: 'GPU0', description: 'Integrated graphics', cpu: false, own_memory: false, memory_total: 0, prompt_ratio: 6.5, token_ratio: 0.8, uses: ['prompt'], status: 'used', badge: 'Prompt', why: 'Reads the prompt 6.5x faster.' },
  ],
  memory: { pools: [] },
};

const BACKENDS = {
  registered: ['CPU', 'Metal'],
  packs: [
    { id: 'cuda', title: 'NVIDIA graphics (CUDA)', size: 520_000_000, applies: true, installed: false },
    { id: 'hip', title: 'AMD graphics (HIP)', size: 610_000_000, applies: false, installed: false },
  ],
};

const FILES: Record<string, [string, string]> = {
  'index.html': [
    'text/html',
    `<!doctype html><html><head><style>body{font-family:system-ui,sans-serif;margin:0;color:#1f2933}.hero{padding:4rem 1.5rem;background:linear-gradient(135deg,#e0f2fe,#f0fdfa)}h1{margin:0 0 .5rem;font-size:2.5rem}button{font:inherit;padding:.6rem 1rem;border-radius:8px;border:0;background:#0e7490;color:#fff}</style></head><body><section class="hero"><h1>Hello</h1><p>Fast, local, private.</p><button id="b">Try it</button><p id="o"></p></section><script>document.getElementById('b').onclick=()=>{document.getElementById('o').textContent='Scripts run inside the preview only.'}</script></body></html>`,
  ],
  'src/style.css': ['text/css', 'body {\n  font-family: system-ui, sans-serif;\n  color: #1f2933;\n  line-height: 1.6;\n}\n\n.hero { padding: 4rem 1rem; }\nh1 { margin: 0; }\n'],
  'README.md': ['text/markdown', '# Website\n\nThe landing page.\n\n- `index.html`: the page\n- `src/style.css`: its styles\n'],
};

const streams = new Set<Res>();
function emit(event: string, data: unknown) {
  const msg = `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`;
  for (const s of streams) s.write(msg);
}
function summary(c: Conv) {
  const { items: _items, ...rest } = c;
  return rest;
}
function touch(c: Conv, state?: string) {
  c.updated = now();
  if (state) c.state = state;
  emit('conversation', { id: c.id, title: c.title, state: c.state, updated: c.updated });
}
function upsert(c: Conv, item: Item) {
  const i = c.items.findIndex((x) => x.id === item.id);
  if (i < 0) c.items.push(item);
  else c.items[i] = item;
  emit('item', { conversation: c.id, item });
}
const wait = (ms: number) => new Promise((r) => setTimeout(r, ms));

// One scripted run: reasoning, two reads, an edit, an answer, streamed as the server would.
async function play(c: Conv) {
  touch(c, 'running');
  const a: Item = { id: id('a'), kind: 'assistant', text: '', reasoning: '', streaming: true };
  upsert(c, a);
  for (const r of ['Looking at ', 'what is asked. ', 'I will read the files first.']) {
    await wait(200);
    a.reasoning += r;
    emit('delta', { conversation: c.id, item: a.id, reasoning: r });
  }
  if (c.workspace) {
    for (const p of ['README.md', 'index.html']) {
      const tl: Item = { id: id('t'), kind: 'tool', tool: 'read_file', title: `Read ${p}`, args: { path: p }, state: 'running' };
      upsert(c, tl);
      await wait(300);
      upsert(c, { ...tl, state: 'done', result: FILES[p][1].slice(0, 200) });
    }
  }
  const words = 'Done. Here is what I found: the page is small and clean, with one stylesheet. Nothing needed changing.'.split(' ');
  for (const w of words) {
    await wait(70);
    const piece = (a.text ? ' ' : '') + w;
    a.text += piece;
    emit('delta', { conversation: c.id, item: a.id, text: piece });
    emit('token', { step: 1, steps: 100, wall_ms: 80 + Math.random() * 20, io_ms: 10, compute_ms: 55, mgmt_ms: 2, stall_ms: 8, flash_wait_ms: 12, read_mib: 40, cache_hit_pct: 72, cache_budget_mib: 7300, rss_mib: 9800, mem_available_mib: 4100, majflt: 0, cpu_ms: 400, dense_resident_frac: 1, mtp_batch: 0 });
  }
  upsert(c, { ...a, streaming: false });
  emit('done', { cancelled: false, tokens: words.length * 2, tok_s: 12.4, load_s: 6.2, prefill_s: 0.9, prefill_tps: 140, prefill_read_mib: 300, prefill_dev_tokens: 0, n_prompt: 126, n_past: 900, cache_hit_pct: 72, read_mib: 900, io_s_tok: 0.01, compute_s_tok: 0.055, stall_s_tok: 0.008, flash_wait_s_tok: 0.012, mgmt_s_tok: 0.002, majflt_tok: 0, cpu_s_tok: 0.4, loop_overhead_s_tok: 0.001, cache_resident_mib: 7000, cache_budget_mib: 7300, cache_evictions: 10, cache_rereads: 2, token_demand_mib: 40, mtp_drafted: 0, mtp_accepted: 0, mtp_decodes: 0, drafted_steps: 0, mtp_draft_s_tok: 0, ttft_s: 1.1 });
  touch(c, 'idle');
}

function body(req: Req): Promise<Json> {
  return new Promise((resolve) => {
    let s = '';
    req.on('data', (c) => (s += new TextDecoder().decode(c)));
    req.on('end', () => {
      try {
        resolve(s ? (JSON.parse(s) as Json) : {});
      } catch {
        resolve({});
      }
    });
  });
}

function send(res: Res, status: number, data: unknown, type = 'application/json') {
  res.statusCode = status;
  res.setHeader('Content-Type', type);
  res.end(typeof data === 'string' && type !== 'application/json' ? data : JSON.stringify(data));
}

async function handle(req: Req, res: Res, next: () => void) {
  const url = new URL(req.url ?? '/', 'http://mock');
  const p = url.pathname;
  const m = req.method ?? 'GET';
  if (!p.startsWith('/api/')) return next();
  const parts = p.split('/').slice(2).map(decodeURIComponent);
  const conv = (cid: string) => conversations.find((c) => c.id === cid);

  if (p === '/api/events') {
    res.statusCode = 200;
    res.setHeader('Content-Type', 'text/event-stream');
    res.setHeader('Cache-Control', 'no-cache');
    res.write(`event: state\ndata: ${JSON.stringify(info())}\n\n`);
    streams.add(res);
    const ping = setInterval(() => res.write(': ping\n\n'), 15000);
    res.on('close', () => {
      clearInterval(ping);
      streams.delete(res);
    });
    return;
  }
  if (p === '/api/info') return send(res, 200, info());
  if (p === '/api/params') return send(res, 200, PARAMS);
  if (p === '/api/config') return send(res, 200, CONFIG);
  if (p === '/api/models') return send(res, 200, MODELS);
  if (p === '/api/plan') return send(res, 200, PLAN);
  if (p === '/api/agent' && m === 'GET')
    return send(res, 200, {
      tools: true,
      commands: true,
      agents: [
        { id: 'builtin', name: 'Built-in', available: true },
        { id: 'claude-code', name: 'Claude Code', available: true, missing: [] },
        { id: 'codex', name: 'Codex CLI', available: false, missing: ['npx'] },
        { id: 'opencode', name: 'OpenCode', available: true, missing: [] },
        { id: 'pi', name: 'Pi', available: false, missing: ['pi'] },
      ],
      mcp: [
        {
          name: 'docs',
          command: 'npx',
          state: 'running',
          tools: [{ name: 'docs_search', description: 'Search the documentation' }],
        },
      ],
    });
  if (p === '/api/workspaces' && m === 'GET') return send(res, 200, workspaces);
  if (p === '/api/workspaces' && m === 'POST') {
    const b = await body(req);
    const path = String(b.path ?? '');
    if (!path.startsWith('/')) return send(res, 400, { error: 'not a folder', code: 'not_a_folder' });
    const w = { id: id('w'), name: path.split('/').filter(Boolean).pop() ?? path, path };
    workspaces.push(w);
    return send(res, 200, w);
  }
  if (p === '/api/conversations' && m === 'GET') return send(res, 200, conversations.map(summary));
  if (p === '/api/conversations' && m === 'POST') {
    const b = await body(req);
    const c: Conv = { id: id('c'), title: String(b.title ?? ''), workspace: (b.workspace as string) ?? null, mode: String(b.mode ?? 'ask'), route: String(b.route ?? 'local'), state: 'idle', updated: now(), task: null, items: [] };
    for (const it of (b.items as Item[] | undefined) ?? []) c.items.push({ ...it, id: id('i') });
    conversations.unshift(c);
    return send(res, 200, summary(c));
  }
  if (parts[0] === 'conversations' && parts[1]) {
    const c = conv(parts[1]);
    if (!c) return send(res, 404, { error: 'no such conversation', code: 'not_found' });
    if (parts.length === 2 && m === 'GET') return send(res, 200, c);
    if (parts.length === 2 && m === 'PATCH') {
      Object.assign(c, await body(req));
      touch(c);
      return send(res, 200, summary(c));
    }
    if (parts.length === 2 && m === 'DELETE') {
      conversations.splice(conversations.indexOf(c), 1);
      return send(res, 200, {});
    }
    if (parts[2] === 'messages') {
      if (c.state === 'running') return send(res, 409, { error: 'a run is going on', code: 'busy' });
      const b = await body(req);
      const u: Item = { id: id('u'), kind: 'user', text: String(b.text ?? '') };
      c.items.push(u);
      if (!c.title) c.title = String(u.text).slice(0, 48);
      setTimeout(() => void play(c), 50);
      return send(res, 202, { item: u });
    }
    if (parts[2] === 'edit') {
      const b = await body(req);
      const i = c.items.findIndex((x) => x.id === b.item);
      if (i >= 0) c.items.splice(i);
      const u: Item = { id: id('u'), kind: 'user', text: String(b.text ?? '') };
      upsert(c, u);
      setTimeout(() => void play(c), 50);
      return send(res, 202, {});
    }
    if (parts[2] === 'approval') {
      const b = await body(req);
      const t = c.items.find((x) => x.id === b.item);
      if (t) {
        const ok = b.decision === 'approve';
        upsert(c, { ...t, state: ok ? 'running' : 'denied' });
        setTimeout(() => {
          if (ok) upsert(c, { ...t, state: 'done', result: '> vite build\n✓ built in 412ms' });
          upsert(c, { id: id('a'), kind: 'assistant', text: ok ? 'The build passes.' : 'I did not run the build, as you asked.', streaming: false });
          touch(c, 'idle');
        }, 900);
      }
      return send(res, 200, {});
    }
    if (parts[2] === 'stop') {
      touch(c, 'idle');
      return send(res, 200, {});
    }
    if (parts[2] === 'file') {
      const f = FILES[url.searchParams.get('path') ?? ''];
      if (!f) return send(res, 404, { error: 'no such file', code: 'not_found' });
      res.setHeader('Content-Security-Policy', "sandbox allow-scripts; default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data: blob:; frame-ancestors 'self'");
      return send(res, 200, f[1], f[0]);
    }
  }
  if (p === '/api/skills' && m === 'GET') return send(res, 200, skills);
  if (parts[0] === 'skills' && m === 'PATCH') {
    const s = skills.find((x) => x.name === parts[1]);
    if (s) s.enabled = Boolean((await body(req)).enabled);
    return send(res, 200, {});
  }
  if (parts[0] === 'skills' && (m === 'POST' || m === 'PUT')) {
    const b = await body(req);
    const s = skills.find((x) => x.name === (parts[1] ?? b.name));
    if (s) Object.assign(s, b);
    else skills.push({ name: String(b.name), title: String(b.name), description: String(b.description), source: 'user', enabled: true, body: String(b.body) });
    return send(res, 200, {});
  }
  if (parts[0] === 'skills' && m === 'DELETE') {
    skills.splice(skills.findIndex((x) => x.name === parts[1]), 1);
    return send(res, 200, {});
  }
  if (p === '/api/tasks' && m === 'GET') return send(res, 200, tasks);
  if (parts[0] === 'tasks') {
    if (parts[2] === 'run') return send(res, 200, {});
    if (m === 'POST') tasks.push({ ...(await body(req)), id: id('t'), last_run: null, next_run: now() } as unknown as (typeof tasks)[number]);
    if (m === 'PUT') Object.assign(tasks.find((x) => x.id === parts[1]) ?? {}, await body(req));
    if (m === 'DELETE') tasks.splice(tasks.findIndex((x) => x.id === parts[1]), 1);
    return send(res, 200, {});
  }
  if (p === '/api/providers' && m === 'GET') return send(res, 200, providers);
  if (parts[0] === 'providers') return send(res, 200, {});
  if (p === '/api/connect') return send(res, 200, connect);
  if (parts[0] === 'connect') return send(res, 200, { file: '/home/me/.config/agent/settings.json', backup: '/home/me/.config/agent/settings.json.bak' });
  if (p === '/api/backends') return send(res, 200, BACKENDS);
  if (parts[0] === 'backends' && parts[2] === 'install') {
    for (let i = 1; i <= 5; i++) setTimeout(() => emit('backend', { id: parts[1], received: i * 104_000_000, total: 520_000_000, state: i === 5 ? 'done' : 'running' }), i * 300);
    await wait(1600);
    BACKENDS.packs[0].installed = true;
    return send(res, 200, { restart: true });
  }
  if (p === '/api/update') return send(res, 200, { current: '0.39.0', latest: '0.40.0', available: true, notes: 'https://example.com/notes' });
  if (p === '/api/update/download') return send(res, 200, { path: '/tmp/installer' });
  return send(res, 404, { error: `the mock has no ${m} ${p}`, code: 'not_found' });
}

// The server's own policy for the UI's files (server/access.cpp), so a preview of the build shows
// what the CSP would refuse. Not in dev mode, where Vite injects styles inline.
const CSP =
  "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; font-src 'self'; connect-src 'self'; " +
  "media-src 'none'; object-src 'none'; frame-src 'self' blob:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'";

/** The mock as a Vite plugin: it answers `/api/*` before the proxy would. */
export function mockServer(): Plugin {
  const install = (server: ViteDevServer | PreviewServer, csp: boolean) => {
    server.middlewares.use((req, res, next) => {
      if (csp && !((req as unknown as Req).url ?? '').includes('/file?')) res.setHeader('Content-Security-Policy', CSP);
      void handle(req as unknown as Req, res as unknown as Res, next);
    });
  };
  return {
    name: 'bmoe-mock-server',
    configureServer: (s) => install(s, false),
    configurePreviewServer: (s) => install(s, true),
  };
}
