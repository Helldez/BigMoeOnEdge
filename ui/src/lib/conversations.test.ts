import { describe, expect, it } from 'vitest';
import { ConversationStore, freshConversation, memoryStore, titleFor } from './conversations';
import { turnMetrics } from './turnMetrics';

describe('conversations', () => {
  it('round-trips through the store, newest first, and deletes', () => {
    const store = new ConversationStore(memoryStore());
    const a = { ...freshConversation(), title: 'first', updatedAt: 1, turns: [{ role: 'user' as const, content: 'hi', reasoning: '', metrics: '' }] };
    const b = { ...freshConversation(), title: 'second', updatedAt: 2, turns: [] };
    store.save(a);
    store.save(b);
    expect(store.list().map((m) => m.title)).toEqual(['second', 'first']);
    expect(store.list()[1].turnCount).toBe(1);
    expect(store.load(a.id)).toEqual(a);
    store.delete(a.id);
    expect(store.load(a.id)).toBeNull();
    expect(store.list().map((m) => m.id)).toEqual([b.id]);
  });

  it('survives a storage that throws', () => {
    const broken = {
      getItem: () => {
        throw new Error('no');
      },
      setItem: () => {
        throw new Error('no');
      },
      removeItem: () => {
        throw new Error('no');
      },
    };
    const store = new ConversationStore(broken);
    expect(() => store.save(freshConversation())).not.toThrow();
    expect(store.list()).toEqual([]);
  });

  it('titles are the first line, cut to fit', () => {
    expect(titleFor('  \n ')).toBe('New chat');
    expect(titleFor('What is a mixture of experts?\nmore')).toBe('What is a mixture of experts?');
    const long = 'a'.repeat(80);
    expect(titleFor(long)).toHaveLength(48);
    expect(titleFor(long).endsWith('…')).toBe(true);
  });
});

describe('turnMetrics', () => {
  const s = {
    cancelled: false,
    tokens: 42,
    tok_s: 3.14159,
    prefill_s: 1.25,
    prefill_tps: 13.6,
    n_prompt: 17,
    n_past: 59,
    cache_hit_pct: 81.4,
    read_mib: 0,
    io_s_tok: 0,
    compute_s_tok: 0,
    stall_s_tok: 0,
    mgmt_s_tok: 0,
    cache_resident_mib: 0,
    cache_budget_mib: 0,
    ttft_s: 1.25,
  };
  it('reads the summary into one line, the same line the phone shows', () => {
    expect(turnMetrics(s, 4096, false)).toBe(
      'decode 3.1 tok/s (42 tok) · prefill 13.6 tok/s (17 tok, 1.3 s) · ctx 59/4096 · cache 81%',
    );
    expect(turnMetrics(s, 0, true)).toBe('decode 3.1 tok/s (42 tok) · prefill 13.6 tok/s (17 tok, 1.3 s) · cache 81% · stopped');
  });
  it('derives the prefill rate where the summary does not carry it', () => {
    expect(turnMetrics({ ...s, prefill_tps: 0 }, 0, false)).toContain('prefill 13.6 tok/s (17 tok, 1.3 s)');
    expect(turnMetrics({ ...s, prefill_s: 0 }, 0, false)).not.toContain('prefill');
  });
});
