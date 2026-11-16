import { describe, expect, it } from 'vitest';
import { ConversationStore, freshConversation, memoryStore, titleFor } from './conversations';

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
