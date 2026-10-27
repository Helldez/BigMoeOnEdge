import { describe, expect, it, vi } from 'vitest';
import { ConversationStore, freshConversation, memoryStore } from './conversations';
import { importBody, importLegacy, legacyConversations } from './legacyImport';

const turn = (role: 'user' | 'assistant', content: string, reasoning = '') => ({ role, content, reasoning, metrics: '' });

describe('legacy import', () => {
  it('turns stored turns into user and assistant items', () => {
    const c = { ...freshConversation(), title: 'Hi', turns: [turn('user', 'hello'), turn('assistant', 'hey', 'thinking'), turn('assistant', '  ')] };
    expect(importBody(c)).toEqual({
      title: 'Hi',
      items: [
        { kind: 'user', text: 'hello' },
        { kind: 'assistant', text: 'hey', reasoning: 'thinking' },
      ],
    });
  });

  it('keeps a decision as the option picked', () => {
    const c = {
      ...freshConversation(),
      turns: [{ ...turn('assistant', ''), choices: [{ label: 'A', text: 'tea', prob: 0.2 }, { label: 'B', text: 'coffee', prob: 0.8 }], best: 1 }],
    };
    expect(importBody(c)?.items).toEqual([{ kind: 'assistant', text: 'B) coffee (80%)' }]);
  });

  it('moves every conversation oldest first and empties the browser', async () => {
    const storage = memoryStore();
    const store = new ConversationStore(storage);
    store.save({ ...freshConversation(), title: 'old', updatedAt: 1, turns: [turn('user', 'a')] });
    store.save({ ...freshConversation(), title: 'new', updatedAt: 2, turns: [turn('user', 'b')] });
    store.save({ ...freshConversation(), title: 'empty', updatedAt: 3, turns: [] });
    const create = vi.fn().mockResolvedValue({});
    expect(await importLegacy(storage, create)).toBe(2);
    expect(create.mock.calls.map((c) => c[0].title)).toEqual(['old', 'new']);
    expect(store.list()).toEqual([]);
  });

  it('keeps what the server did not take, for the next start', async () => {
    const storage = memoryStore();
    const store = new ConversationStore(storage);
    store.save({ ...freshConversation(), title: 'a', updatedAt: 1, turns: [turn('user', 'a')] });
    store.save({ ...freshConversation(), title: 'b', updatedAt: 2, turns: [turn('user', 'b')] });
    const create = vi.fn().mockResolvedValueOnce({}).mockRejectedValueOnce(new Error('offline'));
    expect(await importLegacy(storage, create)).toBe(1);
    expect(legacyConversations(storage).map((x) => x.body?.title)).toEqual(['b']);
  });
});
