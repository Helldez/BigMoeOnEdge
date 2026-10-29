// Wire types for the agent (docs/agent.md) and the desktop app's server routes (docs/desktop-app.md):
// conversations and their items, workspaces, skills, tasks, providers, connected apps, backend packs
// and updates. Shapes and guards only, no behaviour. Fields the contracts leave open are typed open
// (a state the UI does not know yet is shown as its word, never dropped).
import { isObject } from './guards';

export type Mode = 'ask' | 'read_only' | 'full';
export const MODES: readonly Mode[] = ['ask', 'read_only', 'full'];

export type ConversationState = 'idle' | 'running' | 'waiting' | 'error' | (string & {});

/** A time the server reports: an ISO 8601 string or seconds since the epoch. */
export type WireTime = string | number | null | undefined;

export interface Workspace {
  id: string;
  name: string;
  path: string;
}

/** One row of `GET /api/conversations`. */
export interface ConversationSummary {
  id: string;
  title: string;
  /** The workspace's id, or null / "" for a plain chat. */
  workspace: string | null;
  mode: Mode | (string & {});
  /** `local`, or `<provider id>/<model>`. */
  route: string;
  /** Who does the work: `builtin`, or another coding agent's catalog id (GET /api/agent). */
  agent?: string;
  state: ConversationState;
  updated: WireTime;
  /** The task whose run this conversation is, when it is one. */
  task?: string | null;
}

export type ToolState = 'running' | 'waiting' | 'done' | 'denied' | 'error' | (string & {});
export type NoticeLevel = 'info' | 'warn' | 'error' | (string & {});

interface ItemBase {
  id: string;
}
export interface UserItem extends ItemBase {
  kind: 'user';
  text: string;
}
export interface AssistantItem extends ItemBase {
  kind: 'assistant';
  text: string;
  reasoning?: string;
  streaming?: boolean;
}
export interface ToolItem extends ItemBase {
  kind: 'tool';
  tool: string;
  title: string;
  args?: Record<string, unknown>;
  state: ToolState;
  result?: string;
  diff?: string;
  file?: string;
  /** The MCP server whose tool this is (`tool` is then `<server>_<tool>`). */
  mcp?: string;
}
export interface NoticeItem extends ItemBase {
  kind: 'notice';
  text: string;
  level: NoticeLevel;
}
export type Item = UserItem | AssistantItem | ToolItem | NoticeItem;

/** `GET /api/conversations/<id>`: the summary and its items. */
export interface Conversation extends ConversationSummary {
  items: Item[];
}

export interface ConversationEvent {
  id: string;
  title?: string;
  state?: ConversationState;
  updated?: WireTime;
}
export interface ItemEvent {
  conversation: string;
  item: Item;
}
export interface DeltaEvent {
  conversation: string;
  item: string;
  text?: string;
  reasoning?: string;
}

export type SkillSource = 'bundled' | 'user' | 'workspace' | (string & {});
export interface Skill {
  name: string;
  title?: string;
  description: string;
  source: SkillSource;
  enabled: boolean;
  body: string;
}

export type ScheduleKind = 'once' | 'interval' | 'daily' | 'weekly';
export interface Schedule {
  kind: ScheduleKind | (string & {});
  /** `once`: a date and time (ISO 8601); `daily` / `weekly`: a time of day, `HH:MM`. */
  at?: string | null;
  every_minutes?: number | null;
  /** `weekly`: the days, 0 = Sunday to 6 = Saturday. */
  weekdays?: number[] | null;
}
export interface TaskRun {
  at: WireTime;
  conversation?: string | null;
  outcome?: string | null;
}
export interface Task {
  id: string;
  title: string;
  prompt: string;
  workspace: string | null;
  mode: Mode | (string & {});
  route: string;
  schedule: Schedule;
  enabled: boolean;
  last_run?: TaskRun | null;
  next_run?: WireTime;
}
export type TaskDraft = Omit<Task, 'id' | 'last_run' | 'next_run'>;

export interface Provider {
  id: string;
  name: string;
  base_url: string;
  models: string[];
  has_key: boolean;
}
export interface ProviderDraft {
  name: string;
  base_url: string;
  models: string[];
  /** Written once, never read back: absent leaves the saved key as it is. */
  api_key?: string;
}

export interface ConnectClient {
  id: string;
  name: string;
  /** How to connect it, in words. */
  how: string;
  snippet: string;
  /** The settings file the snippet belongs in, when there is one. */
  file?: string | null;
  writable: boolean;
}
export interface ConnectWritten {
  file: string;
  backup?: string | null;
}

/** One backend pack of `GET /api/backends`. */
export interface BackendPack {
  id: string;
  title: string;
  size?: number;
  applies: boolean;
  installed: boolean;
}
export interface Backends {
  /** The backends that registered in this server, by name. */
  registered: string[];
  packs: BackendPack[];
  /** Why the feed could not be read, when it could not. */
  feed_error?: string;
}

export interface UpdateInfo {
  current: string;
  latest: string;
  available: boolean;
  notes?: string;
}
export interface UpdateDownloaded {
  /** The checked installer, for `bmoeDesktop.applyUpdate`. */
  path: string;
}

const isStr = (v: unknown): v is string => typeof v === 'string';

const ITEM_KINDS = new Set(['user', 'assistant', 'tool', 'notice']);

export function isItem(v: unknown): v is Item {
  return isObject(v) && isStr(v.id) && isStr(v.kind) && ITEM_KINDS.has(v.kind);
}

/** An agent a conversation may use (GET /api/agent), and whether its program is installed here. */
export interface AgentChoice {
  id: string;
  name: string;
  available: boolean;
  /** The programs it needs that are not on this computer. */
  missing?: string[];
}

/** What a conversation on this server can do (GET /api/agent). */
/** One MCP server of `GET /api/agent` (local clients only): its tools as the model calls them. */
export interface McpServerInfo {
  name: string;
  command: string;
  state: 'running' | 'stopped' | 'failed' | (string & {});
  error?: string;
  tools: { name: string; description: string }[];
}

export interface AgentCapabilities {
  tools: boolean;
  commands: boolean;
  agents: AgentChoice[];
  mcp?: McpServerInfo[];
}

export const BUILTIN_AGENT = 'builtin';

export function isAgentCapabilities(v: unknown): v is AgentCapabilities {
  return (
    isObject(v) &&
    typeof v.tools === 'boolean' &&
    typeof v.commands === 'boolean' &&
    Array.isArray(v.agents) &&
    v.agents.every((a) => isObject(a) && isStr(a.id) && isStr(a.name) && typeof a.available === 'boolean')
  );
}

export function isConversationSummary(v: unknown): v is ConversationSummary {
  return isObject(v) && isStr(v.id);
}

export function isConversationList(v: unknown): v is ConversationSummary[] {
  return Array.isArray(v) && v.every(isConversationSummary);
}

export function isConversation(v: unknown): v is Conversation {
  return isConversationSummary(v) && Array.isArray((v as { items?: unknown }).items);
}

export function isItemEvent(v: unknown): v is ItemEvent {
  return isObject(v) && isStr(v.conversation) && isItem(v.item);
}

export function isDeltaEvent(v: unknown): v is DeltaEvent {
  return isObject(v) && isStr(v.conversation) && isStr(v.item);
}

export function isConversationEvent(v: unknown): v is ConversationEvent {
  return isObject(v) && isStr(v.id);
}

export const isListOf =
  <T>(each: (v: unknown) => boolean) =>
  (v: unknown): v is T[] =>
    Array.isArray(v) && v.every(each);

export const isWorkspace = (v: unknown): v is Workspace => isObject(v) && isStr(v.id) && isStr(v.path);
export const isSkill = (v: unknown): v is Skill => isObject(v) && isStr(v.name);
export const isTask = (v: unknown): v is Task => isObject(v) && isStr(v.id) && isObject(v.schedule);
export const isProvider = (v: unknown): v is Provider => isObject(v) && isStr(v.id) && Array.isArray(v.models);
export const isConnectClient = (v: unknown): v is ConnectClient => isObject(v) && isStr(v.id) && isStr(v.snippet);
export const isBackends = (v: unknown): v is Backends => isObject(v) && Array.isArray(v.packs);
export const isUpdateInfo = (v: unknown): v is UpdateInfo => isObject(v) && typeof v.available === 'boolean';

/** A wire time as milliseconds since the epoch; NaN when there is none. Seconds and ISO both read. */
export function timeMs(t: WireTime): number {
  if (t === null || t === undefined || t === '') return NaN;
  if (typeof t === 'number') return t < 1e12 ? t * 1000 : t;
  if (/^\d+(\.\d+)?$/.test(t)) return timeMs(Number(t));
  return Date.parse(t);
}
