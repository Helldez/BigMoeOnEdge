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

export type ServerState = 'empty' | 'planning' | 'loading' | 'ready' | 'error';

export interface ModelInfo {
  path: string;
  name: string;
  arch: string;
  n_ctx: number;
  n_expert_used: number;
  think_ctl: string;
  load_s: number;
  /** Expert streaming is on for this session; off, the engine is plain llama.cpp on mmap. */
  streaming: boolean;
  /** What a CPU figure is read against: compute threads, and every thread busy in a decode. */
  compute_threads: number;
  busy_threads: number;
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
  /** The knob's settings label, empty when the knob is not a parameter. */
  label?: string;
  /** A setting the plan decided, one it left (the person's, or unmeasured), or a note that is no setting. */
  group?: 'set' | 'kept' | 'notes' | (string & {});
}

/** One line of the memory ledger: something the run holds, and how. */
export interface PlanMemoryRow {
  name: string;
  /** The row in words; every word on the Plan screen comes from the server. */
  label?: string;
  bytes: number;
  locked: boolean;
  /** Held in turn with the expert cache (during a prompt), not beside it. */
  in_turn: boolean;
}

/** One memory pool: the host's, or a device's that is its own. */
export interface PlanMemoryPool {
  name: string;
  label?: string;
  /** The figures are an even share of a sum the placement reports over several devices. */
  estimated?: boolean;
  cap_bytes: number;
  lockable_cap_bytes: number;
  held_bytes: number;
  locked_bytes: number;
  rows: PlanMemoryRow[];
}

export type PlanDeviceUse = 'prompt' | 'tokens' | 'layers';

export interface PlanDevice {
  name: string;
  description: string;
  cpu: boolean;
  own_memory: boolean;
  memory_total: number;
  /** Rate against the host at prompt width, 0 when unmeasured. */
  prompt_ratio: number;
  token_ratio: number;
  /** The rate in words ("40x the host on a prompt"). */
  rate_text?: string;
  /** Codes: they pick a colour, never a word. */
  uses: (PlanDeviceUse | (string & {}))[];
  status: 'used' | 'unused' | 'refused' | 'unusable' | (string & {});
  /** What the device does, or why it does nothing, in words. */
  badge?: string;
  why: string;
}

export interface PlanLane {
  phase: 'prompt' | 'tokens' | (string & {});
  label?: string;
  on: string[];
}

/** What the last turn of the loaded model measured, beside what the plan predicted. */
export interface PlanMeasured {
  s_per_token: number;
  cache_hit_pct: number;
  tokens: number;
}

/** A `plan_progress` event: a stage of the measurement starting, or `done`. */
export interface PlanProgress {
  step: string;
  label?: string;
  index?: number;
  count?: number;
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
  summary?: { sentence: string; lanes: PlanLane[] };
  predicted?: { s_per_token?: number; cache_hit_pct?: number };
  measured?: PlanMeasured;
  memory?: { pools: PlanMemoryPool[] };
  devices?: PlanDevice[];
  /** Why there is no device beyond the host: '' when there is one. */
  devices_none?: 'build' | 'machine' | 'not_probed' | '' | (string & {});
  /** The same, in words; empty when there is a device. */
  devices_none_text?: string;
  /** Set instead of the fields above when there is nothing to plan (no model selected). */
  error?: string;
}

export type Plan = PlanUnavailable | PlanAvailable;

export interface LocalModel {
  name: string;
  path: string;
  /** The directory it was found in: the models directory or one of `scan_dirs`. */
  dir: string;
  /** Every file of the model that is present, in shard order. */
  files: string[];
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
  /** Added at run time from a repository (`POST /api/hf/add`); it can be removed. */
  added?: boolean;
}

/** One gguf file of a Hugging Face repository (`POST /api/hf/models`). */
export interface HfFile {
  path: string;
  bytes: number;
}

/** One model a repository offers: a gguf, or a split set listed by its first shard. */
export interface HfModel {
  name: string;
  quant: string;
  bytes: number;
  count: number;
  complete: boolean;
  files: HfFile[];
}

export interface HfModelsResponse {
  repo: string;
  revision: string;
  /** The file the request's URL named, "" when it named none. */
  file: string;
  models: HfModel[];
}

/** What a model is before a byte of it is downloaded (`POST /api/hf/inspect`). */
export interface HfFacts {
  repo: string;
  revision: string;
  name: string;
  quant: string;
  bytes: number;
  files: number;
  complete: boolean;
  model: {
    readable: boolean;
    arch: string;
    is_moe: boolean;
    n_expert: number;
    n_expert_used: number;
    n_layer: number;
    n_moe_layer: number;
    n_embd: number;
    dense_bytes: number;
    expert_bytes: number;
    token_cycle_bytes: number;
    largest_dense_tensor: number;
    has_mtp: boolean;
    streamable: boolean;
    refusal: string;
  };
  machine: {
    residency_budget: number;
    memory_total: number;
    disk_free: number;
    present_bytes: number;
  };
  verdict: {
    downloadable: boolean;
    mode: string;
    disk: string;
    memory: string;
  };
}

/** One repository a search found (`POST /api/hf/search`). */
export interface HfRepoHit {
  id: string;
  downloads: number;
  likes: number;
  modified: string;
  gated: boolean;
}

export interface HfSearchResponse {
  query: string;
  repos: HfRepoHit[];
}

/** The catalog entry a model became (`POST /api/hf/add`). */
export interface HfAdded {
  id: string;
  title: string;
  quant: string;
  bytes: number;
}

export interface ModelsResponse {
  models_dir: string;
  /** Other directories whose models are listed and loaded in place (`--scan-dir`). */
  scan_dirs: string[];
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
  /** The flash term that is a share of wall_ms, chosen for the run's mode (io serial, stall overlap); absent from an older server. */
  flash_wait_ms?: number;
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
  /** Per token, the flash term that is a share of the wall (as TokenEvent.flash_wait_ms). */
  flash_wait_s_tok?: number;
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

/** `POST /api/decide`: which of the choices the model would answer next, from one prefill. */
export interface DecideResult {
  cancelled: boolean;
  best: number;
  choice_logp: (number | null)[];
  n_tokens: number;
  n_reused: number;
  n_prefilled: number;
  prefill_s: number;
  prefix_state_mib: number;
}

/** One option of a decision, with the probability the model put on it. */
export interface ChoiceScore {
  label: string;
  text: string;
  prob: number;
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
  /** `code` is what a client decides on (docs/server-api.md); `message` is for a person. */
  error?: { message?: string; code?: string };
}
