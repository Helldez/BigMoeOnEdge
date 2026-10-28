// The conversations the server holds (docs/agent.md): the list for the sidebar, the open one with
// its items, kept current by the `conversation`, `item` and `delta` events of the one event stream
// the app holds (stores/server). A run goes on in the server whether this page is open or not, so
// this store only mirrors it: a reload, or a reconnection, reads the open conversation again.
import { ApiError } from '../lib/api';
import { agentApi, type NewConversation } from '../lib/agentApi';
import {
  BUILTIN_AGENT,
  isConversationEvent,
  isDeltaEvent,
  isItemEvent,
  type AgentCapabilities,
  type ConversationState,
  type ConversationSummary,
  type Item,
  type Mode,
  type Provider,
  type Workspace,
} from '../lib/agent';
import { decideMetrics, decideRequest, parseOptions, scores } from '../lib/decide';
import { isDoneSummary } from '../lib/guards';
import { applyDelta, lastUserItem, truncateFrom, upsertItem, upsertSummary, sortNewest } from '../lib/items';
import { importLegacy } from '../lib/legacyImport';
import type { SseEvent } from '../lib/sse';
import { turnMetrics } from '../lib/turnMetrics';
import { api } from '../lib/api';
import type { ChoiceScore } from '../lib/types';
import { t } from '../lib/text';
import { desktopState } from './desktop.svelte';
import { server } from './server.svelte';

export type ListState = 'loading' | 'ready' | 'error' | 'unavailable';

/** A decision (Choose from options): not a conversation turn, so it is the page's own and not saved. */
export interface Decision {
  id: number;
  question: string;
  options: string[];
  choices: ChoiceScore[] | null;
  best: number;
  metrics: string;
  error: string;
  pending: boolean;
}

export interface Draft {
  workspace: string | null;
  mode: Mode;
  route: string;
  agent: string;
}

const BUSY: ReadonlySet<string> = new Set(['running', 'waiting']);
let nextDecision = 1;

function messageOf(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

class AgentStore {
  conversations = $state<ConversationSummary[]>([]);
  listState = $state<ListState>('loading');
  listError = $state('');
  workspaces = $state<Workspace[]>([]);
  providers = $state<Provider[]>([]);

  /** The open conversation's id; '' while a new one is being written (it exists once sent). */
  currentId = $state('');
  current = $state<ConversationSummary | null>(null);
  items = $state<Item[]>([]);
  loadingCurrent = $state(false);
  currentError = $state('');
  /** A failed action on the open conversation, for a person, and its code for the screen. */
  actionError = $state('');
  actionCode = $state('');
  sending = $state(false);
  /** Approvals sent and not answered by an event yet, by item id. */
  answering = $state<Record<string, boolean>>({});
  /** The one-line figure under a local answer, from the engine's `done` event, by item id. */
  metrics = $state<Record<string, string>>({});
  decisions = $state<Decision[]>([]);
  /** How many conversations the first start moved from this browser to the server. */
  imported = $state(0);

  /** What a new conversation starts with: the composer's chips before the first message. */
  draft = $state<Draft>({ workspace: null, mode: 'ask', route: 'local', agent: BUILTIN_AGENT });
  /** What conversations here can do; until the server says, plain chat with the built-in agent. */
  caps = $state<AgentCapabilities>({ tools: true, commands: false, agents: [] });

  running = $derived(this.current !== null && BUSY.has(this.current.state));
  /** The settings in force for the composer: the open conversation's, else the draft's. */
  workspace = $derived(this.current ? this.current.workspace || null : this.draft.workspace);
  mode = $derived<Mode>(((this.current?.mode as Mode) ?? this.draft.mode) || 'ask');
  route = $derived(this.current?.route ?? this.draft.route);
  agentId = $derived(this.current?.agent ?? this.draft.agent);

  private started = false;
  private opening = 0;

  start(): void {
    if (this.started) return;
    this.started = true;
    server.subscribe((ev) => this.onEvent(ev));
    server.onOpen(() => {
      // Events sent while this page was cut off are gone: read what they changed.
      this.refreshList();
      if (this.currentId) this.reloadCurrent();
    });
    this.refreshList().then(() => this.importOld());
    this.refreshWorkspaces();
    this.refreshProviders();
    this.refreshCapabilities();
  }

  async refreshCapabilities(): Promise<void> {
    try {
      this.caps = await agentApi.capabilities();
    } catch {
      // an older server: the built-in agent alone
    }
  }

  async refreshList(): Promise<void> {
    try {
      this.conversations = sortNewest(await agentApi.conversations());
      this.listState = 'ready';
      this.listError = '';
    } catch (e) {
      if (e instanceof ApiError && (e.status === 404 || e.status === 405)) this.listState = 'unavailable';
      else {
        this.listState = this.listState === 'ready' ? 'ready' : 'error';
        this.listError = messageOf(e);
      }
    }
  }

  async refreshWorkspaces(): Promise<void> {
    try {
      this.workspaces = await agentApi.workspaces();
    } catch {
      // none, or not reachable from here (a phone on the network): plain chat only
    }
  }

  async refreshProviders(): Promise<void> {
    try {
      this.providers = await agentApi.providers();
    } catch {
      // none
    }
  }

  /** Once: the old UI's conversations from this browser, sent to the server and then removed here. */
  private async importOld(): Promise<void> {
    if (this.listState !== 'ready') return;
    let storage: Storage;
    try {
      storage = localStorage;
      if (!storage.getItem('bmoe.conversations')) return;
    } catch {
      return;
    }
    const moved = await importLegacy(storage, (b: NewConversation) => agentApi.createConversation(b));
    if (moved > 0) {
      this.imported = moved;
      await this.refreshList();
    }
  }

  /** A blank conversation: nothing exists on the server until the first message. */
  newConversation(draft?: Partial<Draft>): void {
    this.currentId = '';
    this.current = null;
    this.items = [];
    this.decisions = [];
    this.currentError = '';
    this.clearError();
    if (draft) this.draft = { ...this.draft, ...draft };
  }

  async open(id: string): Promise<void> {
    if (!id) {
      this.newConversation();
      return;
    }
    if (id === this.currentId && this.current) return;
    this.currentId = id;
    this.current = this.conversations.find((c) => c.id === id) ?? null;
    this.items = [];
    this.decisions = [];
    this.clearError();
    await this.reloadCurrent();
  }

  private async reloadCurrent(): Promise<void> {
    const id = this.currentId;
    const ticket = ++this.opening;
    this.loadingCurrent = true;
    this.currentError = '';
    try {
      const c = await agentApi.conversation(id);
      if (ticket !== this.opening) return;
      const { items, ...summary } = c;
      this.current = summary;
      this.items = items;
    } catch (e) {
      if (ticket !== this.opening) return;
      this.currentError = messageOf(e);
    } finally {
      if (ticket === this.opening) this.loadingCurrent = false;
    }
  }

  clearError(): void {
    this.actionError = '';
    this.actionCode = '';
  }

  private fail(e: unknown): void {
    this.actionError = messageOf(e);
    this.actionCode = e instanceof ApiError ? e.code : '';
  }

  /** The open conversation's id, creating it from the draft on the first message. */
  private async ensure(): Promise<string> {
    if (this.currentId) return this.currentId;
    const d = this.draft;
    const c = await agentApi.createConversation(
      d.agent === BUILTIN_AGENT
        ? { workspace: d.workspace, mode: d.mode, route: d.route }
        : { workspace: d.workspace, mode: d.mode, agent: d.agent },
    );
    this.currentId = c.id;
    this.current = c;
    this.conversations = upsertSummary(
      this.conversations.filter((x) => x.id !== c.id).concat(c),
      { id: c.id },
    );
    // The address names the conversation, so a reload opens it again.
    history.replaceState(null, '', `#/chat/${encodeURIComponent(c.id)}`);
    return c.id;
  }

  async send(text: string, think?: boolean): Promise<boolean> {
    const body = text.trim();
    if (!body || this.sending) return false;
    this.sending = true;
    this.clearError();
    try {
      const id = await this.ensure();
      const r = await agentApi.send(id, body, think);
      if (id === this.currentId) {
        this.items = upsertItem(this.items, r.item);
        if (this.current) this.current = { ...this.current, state: 'running' };
      }
      return true;
    } catch (e) {
      this.fail(e);
      return false;
    } finally {
      this.sending = false;
    }
  }

  /** Ask again from a past question: it and everything after it go, then the new text is sent. */
  async edit(itemId: string, text: string): Promise<boolean> {
    const id = this.currentId;
    if (!id || !text.trim()) return false;
    this.clearError();
    try {
      await agentApi.edit(id, itemId, text.trim());
      this.items = truncateFrom(this.items, itemId);
      if (this.current) this.current = { ...this.current, state: 'running' };
      return true;
    } catch (e) {
      this.fail(e);
      return false;
    }
  }

  /** The last answer again: the last question, resent as it was. */
  async regenerate(): Promise<void> {
    const last = lastUserItem(this.items);
    if (last && last.kind === 'user') await this.edit(last.id, last.text);
  }

  async answer(itemId: string, decision: 'approve' | 'deny'): Promise<void> {
    const id = this.currentId;
    if (!id) return;
    this.answering = { ...this.answering, [itemId]: true };
    this.clearError();
    try {
      await agentApi.approval(id, itemId, decision);
    } catch (e) {
      this.fail(e);
      const { [itemId]: _gone, ...rest } = this.answering;
      this.answering = rest;
    }
  }

  async stop(): Promise<void> {
    if (!this.currentId) return;
    try {
      await agentApi.stop(this.currentId);
    } catch (e) {
      this.fail(e);
    }
  }

  /** The composer's chips: a new conversation's draft, or a PATCH of the open one. */
  async setMode(mode: Mode): Promise<void> {
    await this.change({ mode });
  }
  async setRoute(route: string): Promise<void> {
    await this.change({ route });
  }
  async setWorkspace(workspace: string | null): Promise<void> {
    await this.change({ workspace });
  }
  async setAgent(agent: string): Promise<void> {
    await this.change({ agent });
  }

  private async change(p: Partial<Draft>): Promise<void> {
    if (!this.currentId || !this.current) {
      this.draft = { ...this.draft, ...p };
      return;
    }
    const before = this.current;
    this.current = { ...before, ...p } as ConversationSummary;
    try {
      await agentApi.patchConversation(this.currentId, p);
      this.conversations = this.conversations.map((c) => (c.id === before.id ? { ...c, ...p } as ConversationSummary : c));
    } catch (e) {
      this.current = before;
      this.fail(e);
    }
  }

  async rename(id: string, title: string): Promise<void> {
    try {
      await agentApi.patchConversation(id, { title });
      this.conversations = this.conversations.map((c) => (c.id === id ? { ...c, title } : c));
      if (this.current?.id === id) this.current = { ...this.current, title };
    } catch (e) {
      this.fail(e);
    }
  }

  async remove(id: string): Promise<void> {
    try {
      await agentApi.deleteConversation(id);
      this.conversations = this.conversations.filter((c) => c.id !== id);
      if (id === this.currentId) {
        this.newConversation();
        history.replaceState(null, '', '#/chat');
      }
    } catch (e) {
      this.fail(e);
    }
  }

  /** A folder for the workspace: the app's own dialog when there is one, else the path typed. */
  async addWorkspace(path: string): Promise<Workspace | null> {
    try {
      const w = await agentApi.addWorkspace(path);
      this.workspaces = [...this.workspaces.filter((x) => x.id !== w.id), w];
      return w;
    } catch (e) {
      this.fail(e);
      return null;
    }
  }

  /** Choose from options (docs/decide.md): the model picks one; shown under the conversation, not kept. */
  async decide(question: string, optionsText: string): Promise<void> {
    const options = parseOptions(optionsText);
    const d: Decision = { id: nextDecision++, question, options, choices: null, best: -1, metrics: '', error: '', pending: true };
    if (options.length < 2) {
      d.error = t('choose.needTwo');
      d.pending = false;
      this.decisions = [...this.decisions, d];
      return;
    }
    this.decisions = [...this.decisions, d];
    const set = (patch: Partial<Decision>) => (this.decisions = this.decisions.map((x) => (x.id === d.id ? { ...x, ...patch } : x)));
    try {
      const r = await api.decide(decideRequest(question, options));
      if (r.cancelled) set({ pending: false, error: t('choose.cancelled') });
      else set({ pending: false, choices: scores(r, options), best: r.best, metrics: decideMetrics(r) });
    } catch (e) {
      set({ pending: false, error: messageOf(e) });
    }
  }

  dismissDecision(id: number): void {
    this.decisions = this.decisions.filter((d) => d.id !== id);
  }

  private onEvent(ev: SseEvent): void {
    let data: unknown;
    if (ev.event !== 'conversation' && ev.event !== 'item' && ev.event !== 'delta' && ev.event !== 'done') return;
    try {
      data = JSON.parse(ev.data);
    } catch {
      return;
    }
    switch (ev.event) {
      case 'conversation':
        if (isConversationEvent(data)) {
          const before = this.conversations.find((c) => c.id === data.id)?.state ?? this.current?.state;
          this.conversations = upsertSummary(this.conversations, data);
          if (data.id === this.currentId && this.current) {
            this.current = {
              ...this.current,
              title: data.title ?? this.current.title,
              state: data.state ?? this.current.state,
              updated: data.updated ?? this.current.updated,
            };
          }
          if (data.state) this.notifyFinished(data.id, before, data.state);
        }
        break;
      case 'item':
        if (isItemEvent(data)) {
          if (data.conversation === this.currentId) {
            this.items = upsertItem(this.items, data.item);
            if (data.item.kind === 'tool' && data.item.state !== 'waiting' && this.answering[data.item.id]) {
              const { [data.item.id]: _gone, ...rest } = this.answering;
              this.answering = rest;
            }
          }
          if (data.item.kind === 'tool' && data.item.state === 'waiting') this.notifyApproval(data.conversation, data.item.title);
        }
        break;
      case 'delta':
        if (isDeltaEvent(data) && data.conversation === this.currentId) this.items = applyDelta(this.items, data);
        break;
      case 'done':
        // The engine's figures for the turn that just ended: they belong to the open conversation's
        // newest answer when it runs on the local model.
        if (isDoneSummary(data) && this.current && this.route === 'local') {
          for (let i = this.items.length - 1; i >= 0; i--) {
            const it = this.items[i];
            if (it.kind === 'assistant') {
              this.metrics = { ...this.metrics, [it.id]: turnMetrics(data, server.info?.model?.n_ctx ?? 0, data.cancelled) };
              break;
            }
          }
        }
        break;
    }
  }

  private titleOf(id: string): string {
    return this.conversations.find((c) => c.id === id)?.title || t('conversation.untitled');
  }

  private notifyApproval(conversation: string, what: string): void {
    void desktopState.notifyIfAway(t('notify.approval.title'), `${this.titleOf(conversation)}: ${what}`);
  }

  private notifyFinished(id: string, before: ConversationState | undefined, after: ConversationState): void {
    if (!before || !BUSY.has(before) || BUSY.has(after)) return;
    const title = after === 'error' ? t('notify.failed.title') : t('notify.done.title');
    void desktopState.notifyIfAway(title, this.titleOf(id));
  }
}

export const agent = new AgentStore();
