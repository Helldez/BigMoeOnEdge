// The conversations and the turn in flight. The server keeps no history: every request carries
// the whole conversation and the engine reuses the leading tokens its KV cache already holds, so
// a follow-up costs its own message and switching conversations costs a full prefill. What is
// kept here is saved in this browser, one entry per conversation, and survives a reload.
import { api, ApiError, streamChat } from '../lib/api';
import { applyChunk, initialStreamState, type StreamState } from '../lib/chatStream';
import {
  browserStore,
  freshConversation,
  titleFor,
  type Conversation,
  type ConversationMeta,
  type StoredTurn,
} from '../lib/conversations';
import { decideMetrics, decideRequest, parseOptions, scores, userText } from '../lib/decide';
import { turnMetrics } from '../lib/turnMetrics';
import type { ChatMessage, ChoiceScore, DoneSummary } from '../lib/types';
import { server } from './server.svelte';

export interface Turn {
  id: number;
  role: 'user' | 'assistant';
  content: string;
  reasoning: string;
  finishReason: string | null;
  summary: DoneSummary | null;
  historyDropped: boolean;
  error: string;
  /** The one-line figure under a finished answer. */
  metrics: string;
  /** A decision's answer: every option with its probability, `best` the one the model picked. */
  choices?: ChoiceScore[];
  best?: number;
}

let nextId = 1;

class ChatStore {
  turns = $state<Turn[]>([]);
  busy = $state(false);
  error = $state('');
  /** Choose mode: the prompt is a question and the model picks one of the options. */
  choose = $state(false);

  /** Every saved conversation, newest first, and the one open. */
  conversations = $state<ConversationMeta[]>([]);
  current = $state<Conversation>(freshConversation());

  private readonly store = browserStore();
  private abort: AbortController | null = null;

  /** Text of a message that failed before producing anything, for the composer to restore. */
  restore = $state('');

  start(): void {
    this.conversations = this.store.list();
  }

  /**
   * The full history goes back on every request: the server compares it with the previous
   * request plus its own reply and continues the KV cache when they match. A decision is not a
   * conversation turn, so the turns of one stay out of the history.
   */
  private history(): ChatMessage[] {
    const out: ChatMessage[] = [];
    for (let i = 0; i < this.turns.length; i++) {
      const t = this.turns[i];
      if (t.choices) {
        if (out.length && this.turns[i - 1]?.role === 'user') out.pop();
        continue;
      }
      out.push({ role: t.role, content: t.content });
    }
    return out;
  }

  newChat(): void {
    if (this.busy) return;
    this.turns = [];
    this.error = '';
    this.current = freshConversation();
  }

  open(id: string): void {
    if (this.busy) return;
    const c = this.store.load(id);
    if (!c) return;
    this.current = c;
    this.turns = c.turns.map((t) => ({
      id: nextId++,
      role: t.role,
      content: t.content,
      reasoning: t.reasoning,
      finishReason: null,
      summary: null,
      historyDropped: false,
      error: '',
      metrics: t.metrics,
      choices: t.choices,
      best: t.best,
    }));
    this.error = '';
  }

  delete(id: string): void {
    if (this.busy && id === this.current.id) return;
    this.store.delete(id);
    if (id === this.current.id) {
      this.turns = [];
      this.current = freshConversation();
    }
    this.conversations = this.store.list();
  }

  /** Saved after every committed turn: a reload finds the conversation as it was. */
  private persist(): void {
    const stored: StoredTurn[] = this.turns
      .filter((t) => t.content !== '' || t.reasoning !== '' || t.choices)
      .map((t) => ({ role: t.role, content: t.content, reasoning: t.reasoning, metrics: t.metrics, choices: t.choices, best: t.best }));
    if (stored.length === 0) return;
    const firstUser = stored.find((t) => t.role === 'user');
    this.current = {
      ...this.current,
      title: this.current.title || titleFor(firstUser?.content ?? ''),
      updatedAt: Date.now(),
      model: server.info?.model?.name ?? this.current.model,
      turns: stored,
    };
    this.store.save(this.current);
    this.conversations = this.store.list();
  }

  async send(text: string, maxTokens?: number, think?: boolean): Promise<void> {
    const content = text.trim();
    if (!content || this.busy) return;
    this.error = '';
    this.turns.push(this.turn('user', content));
    await this.generate(maxTokens, think);
  }

  /** Drop the last answer and ask again: the KV holds the prompt, so only the answer is paid. */
  async regenerate(): Promise<void> {
    if (this.busy) return;
    const last = this.turns[this.turns.length - 1];
    if (!last || last.role !== 'assistant' || last.choices) return;
    this.turns = this.turns.slice(0, -1);
    this.error = '';
    await this.generate(undefined, undefined);
  }

  private async generate(maxTokens: number | undefined, think: boolean | undefined): Promise<void> {
    const messages = this.history();
    this.turns.push(this.turn('assistant', ''));
    const reply = this.turns[this.turns.length - 1];

    this.busy = true;
    this.abort = new AbortController();
    server.resetMetrics();
    let state: StreamState = initialStreamState();
    try {
      await streamChat(
        { messages, maxTokens, think },
        (chunk) => {
          state = applyChunk(state, chunk);
          this.write(reply.id, state);
        },
        this.abort.signal,
      );
      // A failure after the stream opened arrives as an error chunk, not as a status: handled like
      // one, so a refused turn leaves the history as it was.
      if (state.error) this.fail(reply.id, new Error(state.error));
      else this.persist();
    } catch (e) {
      if (e instanceof DOMException && e.name === 'AbortError') {
        state = { ...state, finishReason: state.finishReason ?? 'cancelled' };
        this.write(reply.id, state);
        this.persist();
      } else {
        this.fail(reply.id, e);
      }
    } finally {
      this.busy = false;
      this.abort = null;
    }
  }

  /**
   * A decision: the question and the options go to `POST /api/decide` as one prefill, and the
   * answer is every option with the probability the model put on it.
   */
  async decide(question: string, optionsText: string): Promise<void> {
    const options = parseOptions(optionsText);
    if (this.busy) return;
    if (options.length < 2) {
      this.error = 'Give at least two options, one per line.';
      return;
    }
    this.error = '';
    this.turns.push(this.turn('user', userText(question.trim(), options)));
    this.turns.push(this.turn('assistant', ''));
    const reply = this.turns[this.turns.length - 1];
    this.busy = true;
    server.resetMetrics();
    try {
      const r = await api.decide(decideRequest(question.trim(), options));
      const t = this.find(reply.id);
      if (!t) return;
      if (r.cancelled) {
        t.finishReason = 'cancelled';
      } else {
        t.choices = scores(r, options);
        t.best = r.best;
        t.metrics = decideMetrics(r);
        this.persist();
      }
    } catch (e) {
      this.fail(reply.id, e);
    } finally {
      this.busy = false;
    }
  }

  /** Stops the generation on the server and drops the local stream. */
  stop(): void {
    if (!this.busy) return;
    api.cancel().catch(() => {});
    this.abort?.abort();
  }

  private turn(role: Turn['role'], content: string): Turn {
    return { id: nextId++, role, content, reasoning: '', finishReason: null, summary: null, historyDropped: false, error: '', metrics: '' };
  }

  private find(id: number): Turn | undefined {
    return this.turns.find((t) => t.id === id);
  }

  private write(id: number, s: StreamState): void {
    const t = this.find(id);
    if (!t) return; // the chat was cleared mid-stream
    t.content = s.content;
    t.reasoning = s.reasoning;
    t.finishReason = s.finishReason;
    t.summary = s.summary;
    t.historyDropped = s.historyDropped;
    if (s.summary) t.metrics = turnMetrics(s.summary, server.info?.model?.n_ctx ?? 0, s.finishReason === 'cancelled' || s.summary.cancelled);
  }

  private fail(id: number, e: unknown): void {
    const t = this.find(id);
    let msg = e instanceof Error ? e.message : String(e);
    if (e instanceof ApiError && e.status === 409) msg = `Busy: another generation is running (${msg}).`;
    if (!t) return;
    if (t.content === '' && t.reasoning === '' && !t.choices) {
      // Nothing was produced (a 409 or a refused request): drop the exchange so the history
      // stays a user/assistant alternation, and hand the text back to the composer.
      const i = this.turns.findIndex((x) => x.id === id);
      const user = i > 0 ? this.turns[i - 1] : undefined;
      this.turns = this.turns.filter((x) => x.id !== id && x !== user);
      if (user) this.restore = user.content;
      this.error = msg;
    } else {
      t.error = msg;
      this.persist();
    }
  }
}

export const chat = new ChatStore();
