// Wire types for the bmoe-server HTTP API (docs/server-api.md). Shapes only, no behaviour.

export type ParamType = 'bool' | 'int' | 'float' | 'choice' | 'path' | 'text';
export type ParamLevel = 'basic' | 'advanced' | 'experimental' | 'debug';
export type ParamScope = 'request' | 'session';
export type ParamValue = boolean | number | string;

export interface ParamChoice {
  value: string;
  label: string;
}

export interface ParamSwitch {
  flag: string;
  value: ParamValue;
  deprecated?: boolean;
}

/** One row of `GET /api/params`. `type`, `level` and `scope` stay open strings for forward compatibility. */
export interface ParamSpec {
  key: string;
  label: string;
  type: ParamType | (string & {});
  group: string;
  group_label: string;
  level: ParamLevel | (string & {});
  scope: ParamScope | (string & {});
  help: string;
  flag: string;
  default: ParamValue;
  short?: string;
  switches?: ParamSwitch[];
  choices?: ParamChoice[];
  min?: number;
  max?: number;
  unit?: string;
  lossy?: boolean;
  accepts_auto?: boolean;
}

export type ServerState = 'empty' | 'loading' | 'ready' | 'error';

export interface ModelInfo {
  path: string;
  name: string;
  arch: string;
  n_ctx: number;
  n_expert_used: number;
  think_ctl: string;
  load_s: number;
  streaming: boolean;
}

export interface Info {
  version: string;
  overlap_available: boolean;
  planner_available: boolean;
  host: { ram_total_mib: number; ram_available_mib: number; cpu_threads: number };
  state: ServerState;
  error: string;
  model: ModelInfo | null;
  generating: boolean;
}

export type PlanSource = 'measured' | 'derived' | 'policy' | 'operator' | 'unprobed';

export interface PlanDecision {
  knob: string;
  value: string;
  source: PlanSource | (string & {});
  reason: string;
}

export interface Config {
  values: Record<string, ParamValue>;
  user_keys: string[];
  args: string[];
  valid: boolean;
  error: string;
  reload_required: boolean;
  /** The last applied plan's decisions, or null. */
  plan: PlanDecision[] | { decisions: PlanDecision[] } | null;
  /** Every load plans first (only true where the planner is built in). */
  auto_plan?: boolean;
  /** Only on a `PUT /api/config` response. */
  rejected?: Record<string, string>;
}

export interface ConfigUpdate {
  values?: Record<string, ParamValue>;
  reset?: string[];
  auto_plan?: boolean;
}

export interface PlanUnavailable {
  available: false;
  reason: string;
}

export interface PlanAvailable {
  available: true;
  regime: string;
  streaming_declined: boolean;
  decline_reason: string;
  decisions: PlanDecision[];
  values: Record<string, ParamValue>;
  args: string[];
  explain: string;
  machine?: string;
  not_applicable?: string[];
  /** Set when the plan was measured with a model loaded, which makes it pessimistic. */
  warning?: string;
  /** The plan applied at the last load, served without measuring again. */
  from_last_load?: boolean;
  /** Set instead of the fields above when there is nothing to plan (no model selected). */
  error?: string;
}

export type Plan = PlanUnavailable | PlanAvailable;

export interface LocalModel {
  name: string;
  path: string;
  bytes: number;
  shards: number;
  arch: string;
  streamable: boolean;
  complete: boolean;
}

export type CatalogStatus = 'on_disk' | 'downloading' | 'available';

export interface CatalogEntry {
  id: string;
  title: string;
  quant: string;
  file: string;
  bytes: number;
  blurb: string;
  status: CatalogStatus | (string & {});
  note: string;
}

export interface ModelsResponse {
  models_dir: string;
  local: LocalModel[];
  catalog: CatalogEntry[];
}

export interface TokenEvent {
  step: number;
  steps: number;
  wall_ms: number;
  io_ms: number;
  compute_ms: number;
  mgmt_ms: number;
  stall_ms: number;
  read_mib: number;
  cache_hit_pct: number;
  cache_budget_mib: number;
  rss_mib: number;
  mem_available_mib: number;
  majflt: number;
  cpu_ms: number;
  dense_resident_frac: number;
  mtp_batch: number;
}

export interface DoneSummary {
  cancelled: boolean;
  tokens: number;
  tok_s: number;
  prefill_s: number;
  prefill_tps: number;
  n_prompt: number;
  n_past: number;
  cache_hit_pct: number;
  read_mib: number;
  io_s_tok: number;
  compute_s_tok: number;
  stall_s_tok: number;
  mgmt_s_tok: number;
  cache_resident_mib: number;
  cache_budget_mib: number;
  ttft_s: number;
}

export type DownloadState = 'running' | 'done' | 'error' | 'cancelled';

export interface DownloadEvent {
  id: string;
  file: string;
  received: number;
  total: number;
  state: DownloadState | (string & {});
  error: string;
}

export type ChatRole = 'system' | 'user' | 'assistant';

export interface ChatMessage {
  role: ChatRole;
  content: string;
}

/** One `chat.completion.chunk` with the `bmoe` extension. */
export interface ChatChunk {
  choices?: {
    index?: number;
    delta?: { role?: string; content?: string | null; reasoning_content?: string | null };
    finish_reason?: string | null;
  }[];
  bmoe?: { reset?: boolean; history_dropped?: boolean; summary?: DoneSummary };
  /** Sent in place of chunks when the generation fails after the stream opened. */
  error?: { message?: string; code?: number };
}
