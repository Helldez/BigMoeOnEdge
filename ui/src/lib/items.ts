// A conversation's items as the server sends them (docs/agent.md, "Items" and "Events"), folded
// into what the screen draws. Pure functions over plain arrays: the store keeps the state, these
// say how an event changes it, and the tests pin the rules.
import { timeMs, type AssistantItem, type ConversationEvent, type ConversationSummary, type DeltaEvent, type Item, type ToolItem, type Workspace } from './agent';

/** An `item` event: the item replaces the one with its id where it stands, or joins at the end. */
export function upsertItem(items: readonly Item[], item: Item): Item[] {
  const i = items.findIndex((x) => x.id === item.id);
  if (i < 0) return [...items, item];
  const out = items.slice();
  out[i] = item;
  return out;
}

/** A `delta` event: what a streaming answer adds. An answer the page has not seen starts here. */
export function applyDelta(items: readonly Item[], d: DeltaEvent): Item[] {
  const i = items.findIndex((x) => x.id === d.item);
  if (i < 0) {
    const fresh: AssistantItem = { id: d.item, kind: 'assistant', text: d.text ?? '', reasoning: d.reasoning ?? '', streaming: true };
    return [...items, fresh];
  }
  const cur = items[i];
  if (cur.kind !== 'assistant') return items as Item[];
  const out = items.slice();
  out[i] = {
    ...cur,
    text: cur.text + (d.text ?? ''),
    reasoning: (cur.reasoning ?? '') + (d.reasoning ?? ''),
    streaming: true,
  };
  return out;
}

/** An edit drops the item and everything after it (`POST .../edit`). */
export function truncateFrom(items: readonly Item[], id: string): Item[] {
  const i = items.findIndex((x) => x.id === id);
  return i < 0 ? items.slice() : items.slice(0, i);
}

/** A `conversation` event on the list: the row changes in place, the list stays newest first. */
export function upsertSummary(list: readonly ConversationSummary[], ev: ConversationEvent): ConversationSummary[] {
  const i = list.findIndex((c) => c.id === ev.id);
  let out: ConversationSummary[];
  if (i < 0) {
    out = [
      ...list,
      { id: ev.id, title: ev.title ?? '', workspace: null, mode: 'ask', route: 'local', state: ev.state ?? 'idle', updated: ev.updated ?? null },
    ];
  } else {
    out = list.slice();
    const c = out[i];
    out[i] = {
      ...c,
      title: ev.title ?? c.title,
      state: ev.state ?? c.state,
      updated: ev.updated ?? c.updated,
    };
  }
  return sortNewest(out);
}

export function sortNewest(list: readonly ConversationSummary[]): ConversationSummary[] {
  const t = (c: ConversationSummary) => {
    const ms = timeMs(c.updated);
    return Number.isNaN(ms) ? 0 : ms;
  };
  return list.slice().sort((a, b) => t(b) - t(a));
}

/** What the transcript draws: a message or a notice on its own, a run of tool calls as one group. */
export type Block =
  | { kind: 'item'; item: Item }
  | { kind: 'run'; id: string; items: Item[] };

/**
 * Consecutive tool items become one run, so a turn that read ten files reads as one line
 * with its summary; everything else stands alone, in the server's order.
 */
export function blocks(items: readonly Item[]): Block[] {
  const out: Block[] = [];
  let run: Item[] = [];
  const flush = () => {
    if (run.length === 0) return;
    out.push({ kind: 'run', id: run[0].id, items: run });
    run = [];
  };
  for (const it of items) {
    if (it.kind === 'tool') {
      run.push(it);
      continue;
    }
    // The turn that only called tools leaves an assistant item with nothing to read once it has
    // finished: it is not drawn, so the calls around it stay one run.
    if (it.kind === 'assistant' && !it.streaming && !it.text?.trim() && !it.reasoning?.trim()) continue;
    flush();
    out.push({ kind: 'item', item: it });
  }
  flush();
  return out;
}

/** What a run did, counted: the words come from the dictionary (`summaryParts`). */
export interface RunSummary {
  read: number;
  listed: number;
  searched: number;
  created: number;
  edited: number;
  commands: number;
  pages: number;
  skills: number;
  other: number;
  added: number;
  removed: number;
  /** Tool calls still running or waiting for an answer. */
  pending: number;
  failed: number;
}

/** Lines added and removed by a unified diff (the `+++` and `---` headers are not lines). */
export function diffStat(diff: string | undefined): { added: number; removed: number } {
  let added = 0;
  let removed = 0;
  if (!diff) return { added, removed };
  for (const line of diff.split('\n')) {
    if (line.startsWith('+++') || line.startsWith('---')) continue;
    if (line.startsWith('+')) added++;
    else if (line.startsWith('-')) removed++;
  }
  return { added, removed };
}

function toolPath(t: ToolItem): string {
  const a = t.args ?? {};
  const p = a.path ?? a.file ?? t.file;
  return typeof p === 'string' ? p : '';
}

/** Another agent's tool calls come with the Agent Client Protocol's kind: counted as the built-in tool that does the same. */
const ACP_KIND_AS_TOOL: Record<string, string> = {
  read: 'read_file',
  edit: 'edit_file',
  delete: 'edit_file',
  move: 'edit_file',
  search: 'grep_search',
  execute: 'exec_shell_command',
};

export function runSummary(items: readonly Item[]): RunSummary {
  const s: RunSummary = {
    read: 0,
    listed: 0,
    searched: 0,
    created: 0,
    edited: 0,
    commands: 0,
    pages: 0,
    skills: 0,
    other: 0,
    added: 0,
    removed: 0,
    pending: 0,
    failed: 0,
  };
  // A file read twice, or edited three times, is one file: the summary counts what was touched.
  const read = new Set<string>();
  const edited = new Set<string>();
  const created = new Set<string>();
  for (const it of items) {
    if (it.kind !== 'tool') continue;
    if (it.state === 'running' || it.state === 'waiting') s.pending++;
    if (it.state === 'error') s.failed++;
    const ok = it.state !== 'denied' && it.state !== 'error';
    const path = toolPath(it) || it.id;
    switch (ACP_KIND_AS_TOOL[it.tool] ?? it.tool) {
      case 'read_file':
        read.add(path);
        break;
      case 'file_glob_search':
        s.listed++;
        break;
      case 'grep_search':
        s.searched++;
        break;
      case 'write_file':
        if (ok) created.add(path);
        break;
      case 'edit_file':
        if (ok) edited.add(path);
        break;
      case 'exec_shell_command':
        s.commands++;
        break;
      // Another agent's web reads: this server's own agent reaches the web through an MCP server.
      case 'fetch':
        s.pages++;
        break;
      case 'use_skill':
        s.skills++;
        break;
      default:
        s.other++;
    }
    if (ok) {
      const d = diffStat(it.diff);
      s.added += d.added;
      s.removed += d.removed;
    }
  }
  s.read = read.size;
  s.edited = edited.size;
  s.created = created.size;
  return s;
}

/** A translator: a key and its values, to words (lib/text). */
export type Translate = (key: string, vars?: Record<string, string | number>) => string;

/**
 * The run in one line: "Read 3 files, edited 2 files · +14 −3". The counts pick the plural form
 * (`<key>.one` / `<key>.other`); the first part is capitalised by the dictionary's own `.first` form.
 */
export function summaryText(s: RunSummary, t: Translate): string {
  const counts: [string, number][] = [
    ['run.read', s.read],
    ['run.edited', s.edited],
    ['run.created', s.created],
    ['run.listed', s.listed],
    ['run.searched', s.searched],
    ['run.commands', s.commands],
    ['run.pages', s.pages],
    ['run.skills', s.skills],
    ['run.other', s.other],
  ];
  const parts: string[] = [];
  for (const [key, n] of counts) {
    if (n <= 0) continue;
    const form = n === 1 ? 'one' : 'other';
    parts.push(t(`${key}.${parts.length === 0 ? 'first' : 'next'}.${form}`, { n }));
  }
  let line = parts.join(', ');
  if (s.added > 0 || s.removed > 0) line += `${line ? ' · ' : ''}+${s.added} −${s.removed}`;
  return line;
}

/** The tool call that waits for a person's answer, if one does. */
export function waitingApproval(items: readonly Item[]): ToolItem | null {
  for (const it of items) if (it.kind === 'tool' && it.state === 'waiting') return it;
  return null;
}

/** The last thing the person asked, for Regenerate. */
export function lastUserItem(items: readonly Item[]): Item | null {
  for (let i = items.length - 1; i >= 0; i--) if (items[i].kind === 'user') return items[i];
  return null;
}

/** The newest file the agent produced or changed, for the preview. */
export function latestFile(items: readonly Item[]): string {
  for (let i = items.length - 1; i >= 0; i--) {
    const it = items[i];
    if (it.kind === 'tool' && it.file && it.state === 'done') return it.file;
  }
  return '';
}

export type DateBucket = 'today' | 'yesterday' | 'week' | 'month' | 'older';

export function dateBucket(ms: number, now: number): DateBucket {
  if (Number.isNaN(ms)) return 'older';
  const day = new Date(now);
  day.setHours(0, 0, 0, 0);
  const start = day.getTime();
  const DAY = 86_400_000;
  if (ms >= start) return 'today';
  if (ms >= start - DAY) return 'yesterday';
  if (ms >= start - 6 * DAY) return 'week';
  if (ms >= start - 29 * DAY) return 'month';
  return 'older';
}

export interface ConversationGroup {
  /** `ws:<id>` for a workspace's conversations, `date:<bucket>` for plain chats by date. */
  key: string;
  workspace: Workspace | null;
  bucket: DateBucket | null;
  items: ConversationSummary[];
}

/**
 * The sidebar's list: each workspace's conversations together (the workspace with the newest
 * conversation first), then the chats with no workspace by date. A conversation whose workspace the
 * server no longer lists is a plain chat here.
 */
export function groupConversations(
  list: readonly ConversationSummary[],
  workspaces: readonly Workspace[],
  now: number,
  query = '',
): ConversationGroup[] {
  const q = query.trim().toLowerCase();
  const sorted = sortNewest(list.filter((c) => !q || (c.title || '').toLowerCase().includes(q)));
  const byWs = new Map<string, ConversationGroup>();
  const byDate = new Map<DateBucket, ConversationGroup>();
  const wsGroups: ConversationGroup[] = [];
  for (const c of sorted) {
    const ws = c.workspace ? workspaces.find((w) => w.id === c.workspace) : undefined;
    if (ws) {
      let g = byWs.get(ws.id);
      if (!g) {
        g = { key: `ws:${ws.id}`, workspace: ws, bucket: null, items: [] };
        byWs.set(ws.id, g);
        wsGroups.push(g);
      }
      g.items.push(c);
      continue;
    }
    const b = dateBucket(timeMs(c.updated), now);
    let g = byDate.get(b);
    if (!g) {
      g = { key: `date:${b}`, workspace: null, bucket: b, items: [] };
      byDate.set(b, g);
    }
    g.items.push(c);
  }
  const order: DateBucket[] = ['today', 'yesterday', 'week', 'month', 'older'];
  return [...wsGroups, ...order.flatMap((b) => (byDate.has(b) ? [byDate.get(b)!] : []))];
}
