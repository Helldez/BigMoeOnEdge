// Conversations, kept in this browser. The server holds none: every request carries the
// conversation whole, so what is kept here is the person's own record of it, one entry per
// conversation, read whole when opened and written whole on every change. Pure over a Storage,
// so it is tested with a plain object and survives a browser that offers none.
import type { ChoiceScore } from './types';

export interface StoredTurn {
  role: 'user' | 'assistant';
  content: string;
  reasoning: string;
  /** The one-line figure shown under a finished answer. */
  metrics: string;
  /** A decision's answer: the options with their probabilities, `best` the one picked. */
  choices?: ChoiceScore[];
  best?: number;
}

export interface Conversation {
  id: string;
  title: string;
  createdAt: number;
  updatedAt: number;
  /** The model it was last answered by, for the list. */
  model: string;
  turns: StoredTurn[];
}

/** What the list shows of each conversation: no turns, so a long history lists quickly. */
export interface ConversationMeta {
  id: string;
  title: string;
  updatedAt: number;
  turnCount: number;
  model: string;
}

const INDEX_KEY = 'bmoe.conversations';
const ITEM_PREFIX = 'bmoe.conversation.';

export function newId(): string {
  const rnd = typeof crypto !== 'undefined' && 'randomUUID' in crypto ? crypto.randomUUID() : null;
  return rnd ?? `${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
}

export function freshConversation(): Conversation {
  const now = Date.now();
  return { id: newId(), title: '', createdAt: now, updatedAt: now, model: '', turns: [] };
}

/** A title from the first thing the person said: the first line, cut to fit a list row. */
export function titleFor(firstUserText: string): string {
  const line = firstUserText
    .split('\n')
    .map((l) => l.trim())
    .find((l) => l !== '');
  if (!line) return 'New chat';
  return line.length <= 48 ? line : line.slice(0, 47).trimEnd() + '…';
}

export function metaOf(c: Conversation): ConversationMeta {
  return { id: c.id, title: c.title || 'New chat', updatedAt: c.updatedAt, turnCount: c.turns.length, model: c.model };
}

/** The subset of Storage this uses; `localStorage` is one, and so is a Map-backed stub in tests. */
export interface KeyStore {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
  removeItem(key: string): void;
}

/** A store that keeps nothing, for a browser whose storage is unavailable: the chat still works. */
export const memoryStore = (): KeyStore => {
  const m = new Map<string, string>();
  return {
    getItem: (k) => m.get(k) ?? null,
    setItem: (k, v) => void m.set(k, v),
    removeItem: (k) => void m.delete(k),
  };
};

export class ConversationStore {
  constructor(private readonly storage: KeyStore) {}

  /** Every conversation, newest first. */
  list(): ConversationMeta[] {
    return this.index()
      .map((m) => m)
      .sort((a, b) => b.updatedAt - a.updatedAt);
  }

  load(id: string): Conversation | null {
    const raw = this.get(ITEM_PREFIX + id);
    if (!raw) return null;
    try {
      const c = JSON.parse(raw) as Conversation;
      return c && typeof c === 'object' && Array.isArray(c.turns) ? c : null;
    } catch {
      return null;
    }
  }

  save(c: Conversation): void {
    this.set(ITEM_PREFIX + c.id, JSON.stringify(c));
    const index = this.index().filter((m) => m.id !== c.id);
    index.push(metaOf(c));
    this.set(INDEX_KEY, JSON.stringify(index));
  }

  delete(id: string): void {
    this.remove(ITEM_PREFIX + id);
    this.set(INDEX_KEY, JSON.stringify(this.index().filter((m) => m.id !== id)));
  }

  private index(): ConversationMeta[] {
    const raw = this.get(INDEX_KEY);
    if (!raw) return [];
    try {
      const v: unknown = JSON.parse(raw);
      return Array.isArray(v) ? (v as ConversationMeta[]).filter((m) => m && typeof m.id === 'string') : [];
    } catch {
      return [];
    }
  }

  // Storage can throw (a private window, a full quota): every access is guarded, and a failed
  // write loses that write and nothing else.
  private get(k: string): string | null {
    try {
      return this.storage.getItem(k);
    } catch {
      return null;
    }
  }
  private set(k: string, v: string): void {
    try {
      this.storage.setItem(k, v);
    } catch {
      // kept in memory for this page
    }
  }
  private remove(k: string): void {
    try {
      this.storage.removeItem(k);
    } catch {
      // nothing to remove
    }
  }
}

/** The browser's storage when it answers, a memory store otherwise. */
export function browserStore(): ConversationStore {
  try {
    const probe = '__bmoe_probe__';
    localStorage.setItem(probe, '1');
    localStorage.removeItem(probe);
    return new ConversationStore(localStorage);
  } catch {
    return new ConversationStore(memoryStore());
  }
}
