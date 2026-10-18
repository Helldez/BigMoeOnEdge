// The conversations and the turn in flight. The server keeps no history: every request carries
// the whole conversation and the engine reuses the leading tokens its KV cache already holds, so
// a follow-up costs its own message and switching conversations costs a full prefill. What is
// kept here is saved in this browser, one entry per conversation, and survives a reload.
import { api, ApiError, CODE_CONTEXT_FULL, CODE_NO_MODEL, streamChat } from '../lib/api';
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
  /** A turn that failed before producing anything, for a person; shown under the turn it belongs to. */
  error = $state('');
  /** The error's code (docs/server-api.md), what the screen decides on; empty when it had none. */
  errorCode = $state('');
  /** The error is a full context: the fix is a new chat, and the screen offers one. */
  contextFull = $derived(this.errorCode === CODE_CONTEXT_FULL);
  /** The error is a server without a model: the fix is on Models, and the screen links there. */
  noModel = $derived(this.errorCode === CODE_NO_MODEL);
  /** The failed turn was a message, still last in the conversation: it can be sent again as it is. */
  retryable = $state(false);
  /** Choose mode: the prompt is a question and the model picks one of the options. */
  choose = $state(false);

  /** Every saved conversation, newest first, and the one open. */
  conversations = $state<ConversationMeta[]>([]);
  current = $state<Conversation>(freshConversation());

  private readonly store = browserStore();
  private abort: AbortController | null = null;

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
    this.clearError();
    this.current = freshConversation();
  }

  clearError(): void {
    this.error = '';
    this.errorCode = '';
    this.retryable = false;
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
    this.clearError();
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
    this.clearError();
    this.turns.push(this.turn('user', content));
    await this.generate(maxTokens, think);
  }

  /** Send the conversation again as it stands, after a turn that failed left the message last. */
  async retry(): Promise<void> {
    const last = this.turns[this.turns.length - 1];
    if (this.busy || !this.retryable || !last || last.role !== 'user') return;
    this.clearError();
    await this.generate(undefined, undefined);
  }

  /** Drop the last answer and ask again: the KV holds the prompt, so only the answer is paid. */
  async regenerate(): Promise<void> {
    if (this.busy) return;
    const last = this.turns[this.turns.length - 1];
    if (!last || last.role !== 'assistant' || last.choices) return;
    this.turns = this.turns.slice(0, -1);
    this.clearError();
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
      if (state.error) this.fail(reply.id, new Error(state.error), state.errorCode, true);
      else this.persist();
    } catch (e) {
      if (e instanceof DOMException && e.name === 'AbortError') {
        state = { ...state, finishReason: state.finishReason ?? 'cancelled' };
        this.write(reply.id, state);
        this.persist();
      } else {
        this.fail(reply.id, e, undefined, true);
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
      this.clearError();
      this.error = 'Give at least two options, one per line.';
      return;
    }
    this.clearError();
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

  /**
   * A turn failed. With nothing produced (a refused request, a server that went away), the empty
   * answer is dropped and the person's message stays in the transcript with the error under it,
   * saved, so it can be sent again after the fix (docs/screens.md). With an answer under way, the
   * error is the answer's own and what streamed is kept. `isMessage` is false for a decision.
   */
  private fail(id: number, e: unknown, code = e instanceof ApiError ? e.code : '', isMessage = false): void {
    const t = this.find(id);
    let msg = e instanceof Error ? e.message : String(e);
    if (e instanceof ApiError && e.status === 409) msg = `Busy: another generation is running (${msg}).`;
    if (!t) return;
    if (t.content === '' && t.reasoning === '' && !t.choices) {
      // A decision's question is not a conversation turn: without its answer it would be sent as
      // history on the next message, so it goes with the empty answer.
      const i = this.turns.findIndex((x) => x.id === id);
      const question = !isMessage && i > 0 ? this.turns[i - 1] : undefined;
      this.turns = this.turns.filter((x) => x.id !== id && x !== question);
      this.error = msg;
      this.errorCode = code;
      this.retryable = isMessage && this.turns[this.turns.length - 1]?.role === 'user';
    } else {
      t.error = msg;
    }
    this.persist();
  }
}

export const chat = new ChatStore();
