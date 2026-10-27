// The conversations the earlier web UI kept in this browser, moved to the server once. The server
// now holds conversations (docs/agent.md), so a person who upgrades finds their old chats in the
// sidebar instead of losing them; each is sent as a new conversation with its messages, and the
// browser's copy is removed only after the server took it.
import type { NewConversation } from './agentApi';
import { ConversationStore, type Conversation, type KeyStore } from './conversations';

/** One stored conversation as the body `POST /api/conversations` takes; null when it holds nothing. */
export function importBody(c: Conversation): NewConversation | null {
  const items: NonNullable<NewConversation['items']> = [];
  for (const turn of c.turns) {
    if (turn.choices) {
      // A decision was an answer with probabilities: kept as the option picked, in words.
      const best = turn.best !== undefined ? turn.choices[turn.best] : undefined;
      if (best) items.push({ kind: 'assistant', text: `${best.label}) ${best.text} (${Math.round(best.prob * 100)}%)` });
      continue;
    }
    if (!turn.content.trim() && !turn.reasoning.trim()) continue;
    if (turn.role === 'user') items.push({ kind: 'user', text: turn.content });
    else items.push(turn.reasoning ? { kind: 'assistant', text: turn.content, reasoning: turn.reasoning } : { kind: 'assistant', text: turn.content });
  }
  if (items.length === 0) return null;
  return { title: c.title || undefined, items };
}

/** Every stored conversation with something in it, oldest first so the server's order matches. */
export function legacyConversations(storage: KeyStore): { id: string; body: NewConversation | null }[] {
  const store = new ConversationStore(storage);
  return store
    .list()
    .slice()
    .reverse()
    .map((m) => {
      const c = store.load(m.id);
      return { id: m.id, body: c ? importBody(c) : null };
    });
}

/**
 * Sends each stored conversation with `create`, removing it from the browser once the server has it.
 * A failure leaves that one (and the rest) for the next start. Answers how many moved.
 */
export async function importLegacy(storage: KeyStore, create: (b: NewConversation) => Promise<unknown>): Promise<number> {
  const store = new ConversationStore(storage);
  let moved = 0;
  for (const { id, body } of legacyConversations(storage)) {
    if (body) {
      try {
        await create(body);
      } catch {
        return moved;
      }
      moved++;
    }
    store.delete(id);
  }
  return moved;
}
