import { describe, expect, it } from 'vitest';
import type { ConversationSummary, Item, ToolItem } from './agent';
import { t } from './text';
import {
  applyDelta,
  blocks,
  dateBucket,
  diffStat,
  groupConversations,
  latestFile,
  runSummary,
  summaryText,
  truncateFrom,
  upsertItem,
  upsertSummary,
  waitingApproval,
} from './items';

const tool = (id: string, name: string, extra: Partial<ToolItem> = {}): ToolItem => ({ id, kind: 'tool', tool: name, title: name, state: 'done', ...extra });

describe('the item reducer', () => {
  it('appends a new item and replaces a known one where it stands', () => {
    let items: Item[] = [];
    items = upsertItem(items, { id: 'u1', kind: 'user', text: 'hi' });
    items = upsertItem(items, { id: 'a1', kind: 'assistant', text: '', streaming: true });
    items = upsertItem(items, tool('t1', 'read_file', { state: 'running' }));
    items = upsertItem(items, { id: 'a1', kind: 'assistant', text: 'done', streaming: false });
    expect(items.map((i) => i.id)).toEqual(['u1', 'a1', 't1']);
    expect(items[1]).toMatchObject({ text: 'done', streaming: false });
  });

  it('appends deltas to the answer, and starts one it has not seen', () => {
    let items: Item[] = [{ id: 'u1', kind: 'user', text: 'hi' }];
    items = applyDelta(items, { conversation: 'c', item: 'a1', reasoning: 'Let me ' });
    items = applyDelta(items, { conversation: 'c', item: 'a1', reasoning: 'think.' });
    items = applyDelta(items, { conversation: 'c', item: 'a1', text: 'Hel' });
    items = applyDelta(items, { conversation: 'c', item: 'a1', text: 'lo' });
    expect(items[1]).toEqual({ id: 'a1', kind: 'assistant', text: 'Hello', reasoning: 'Let me think.', streaming: true });
    // The whole item that follows the deltas wins.
    items = upsertItem(items, { id: 'a1', kind: 'assistant', text: 'Hello!', reasoning: 'Let me think.', streaming: false });
    expect(items[1]).toMatchObject({ text: 'Hello!', streaming: false });
  });

  it('ignores a delta for an item that is not an answer', () => {
    const items: Item[] = [{ id: 'u1', kind: 'user', text: 'hi' }];
    expect(applyDelta(items, { conversation: 'c', item: 'u1', text: 'x' })).toEqual(items);
  });

  it('drops an edited question and everything after it', () => {
    const items: Item[] = [
      { id: 'u1', kind: 'user', text: 'a' },
      { id: 'a1', kind: 'assistant', text: 'b' },
      { id: 'u2', kind: 'user', text: 'c' },
      { id: 'a2', kind: 'assistant', text: 'd' },
    ];
    expect(truncateFrom(items, 'u2').map((i) => i.id)).toEqual(['u1', 'a1']);
    expect(truncateFrom(items, 'nope')).toHaveLength(4);
  });
});

describe('the conversation list', () => {
  const list: ConversationSummary[] = [
    { id: 'a', title: 'A', workspace: null, mode: 'ask', route: 'local', state: 'idle', updated: '2026-10-27T10:00:00Z' },
    { id: 'b', title: 'B', workspace: null, mode: 'ask', route: 'local', state: 'idle', updated: '2026-10-27T09:00:00Z' },
  ];
  it('updates a row in place and keeps newest first', () => {
    const out = upsertSummary(list, { id: 'b', state: 'running', updated: '2026-10-27T11:00:00Z' });
    expect(out.map((c) => c.id)).toEqual(['b', 'a']);
    expect(out[0]).toMatchObject({ title: 'B', state: 'running' });
  });
  it('adds a conversation it did not list', () => {
    expect(upsertSummary(list, { id: 'c', title: 'C', updated: 1_900_000_000 }).map((c) => c.id)).toEqual(['c', 'a', 'b']);
  });
  it('groups by workspace first, then plain chats by date', () => {
    const now = Date.parse('2026-10-27T12:00:00');
    const rows: ConversationSummary[] = [
      { ...list[0], id: '1', updated: Date.parse('2026-10-27T08:00:00') },
      { ...list[0], id: '2', updated: Date.parse('2026-10-26T08:00:00') },
      { ...list[0], id: '3', workspace: 'w', updated: Date.parse('2026-10-01T08:00:00') },
      { ...list[0], id: '4', workspace: 'gone', updated: Date.parse('2026-01-01T08:00:00') },
    ];
    const g = groupConversations(rows, [{ id: 'w', name: 'site', path: '/p/site' }], now);
    expect(g.map((x) => x.key)).toEqual(['ws:w', 'date:today', 'date:yesterday', 'date:older']);
    expect(groupConversations(rows, [], now, 'zzz')).toEqual([]);
  });
  it('buckets dates', () => {
    const now = Date.parse('2026-10-27T12:00:00');
    expect(dateBucket(Date.parse('2026-10-24T12:00:00'), now)).toBe('week');
    expect(dateBucket(Date.parse('2026-10-10T12:00:00'), now)).toBe('month');
    expect(dateBucket(NaN, now)).toBe('older');
  });
});

describe('runs of tool calls', () => {
  const diff = '--- a/x\n+++ b/x\n@@ -1,3 +1,4 @@\n-old\n+new\n+more\n same';
  const run: Item[] = [
    tool('1', 'read_file', { args: { path: 'a.ts' } }),
    tool('2', 'read_file', { args: { path: 'b.ts' } }),
    tool('3', 'read_file', { args: { path: 'a.ts' } }),
    tool('4', 'edit_file', { args: { path: 'a.ts' }, diff }),
    tool('5', 'edit_file', { args: { path: 'c.ts' }, diff }),
  ];

  it('groups consecutive tool calls between messages', () => {
    const items: Item[] = [{ id: 'u', kind: 'user', text: 'go' }, ...run, { id: 'a', kind: 'assistant', text: 'ok' }];
    const b = blocks(items);
    expect(b.map((x) => x.kind)).toEqual(['item', 'run', 'item']);
    expect(b[1].kind === 'run' && b[1].items).toHaveLength(5);
  });

  it('keeps a run whole across the empty turns that only called tools', () => {
    const items: Item[] = [
      { id: 'u', kind: 'user', text: 'go' },
      { id: 'e1', kind: 'assistant', text: '', reasoning: '' },
      run[0],
      { id: 'e2', kind: 'assistant', text: '  ', reasoning: '' },
      run[1],
      { id: 's', kind: 'assistant', text: '', reasoning: '', streaming: true },
    ];
    const b = blocks(items);
    expect(b.map((x) => x.kind)).toEqual(['item', 'run', 'item']);
    expect(b[1].kind === 'run' && b[1].items).toHaveLength(2);
  });

  it('counts files touched once each, and lines changed', () => {
    expect(diffStat(diff)).toEqual({ added: 2, removed: 1 });
    const s = runSummary(run);
    expect(s).toMatchObject({ read: 2, edited: 2, added: 4, removed: 2, pending: 0 });
  });

  it('says a run in one line', () => {
    expect(summaryText(runSummary(run), t)).toBe('Read 2 files, edited 2 files · +4 −2');
    expect(summaryText(runSummary([tool('1', 'exec_shell_command')]), t)).toBe('Ran 1 command');
    expect(
      summaryText(runSummary([tool('1', 'file_glob_search'), tool('2', 'grep_search'), tool('3', 'read_file')]), t),
    ).toBe('Read 1 file, looked in 1 folder, searched the files once');
    expect(summaryText(runSummary([tool('1', 'docs_search')]), t)).toBe('Took 1 other step');
  });

  it("counts another agent's tool calls by their protocol kind", () => {
    expect(summaryText(runSummary([tool('1', 'execute'), tool('2', 'read'), tool('3', 'think'), tool('4', 'fetch')]), t)).toBe(
      'Read 1 file, ran 1 command, opened 1 web page, took 1 other step',
    );
  });

  it('does not count a denied change as made', () => {
    const s = runSummary([tool('1', 'edit_file', { state: 'denied', diff })]);
    expect(s.edited).toBe(0);
    expect(s.added).toBe(0);
  });

  it('finds the call waiting for an answer and the newest file', () => {
    const items: Item[] = [tool('1', 'write_file', { file: 'out/index.html' }), tool('2', 'exec_shell_command', { state: 'waiting' })];
    expect(waitingApproval(items)?.id).toBe('2');
    expect(latestFile(items)).toBe('out/index.html');
  });
});
