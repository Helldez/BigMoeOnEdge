// Typed client for the bmoe-server HTTP API. Paths are relative so the UI works from any mount.
import { SseParser, isDoneSentinel } from './sse';
import type { SseEvent } from './sse';
import { auth } from './auth';
import { parseChunk } from './chatStream';
import { isConfig, isDecideResult, isInfo, isModelsResponse, isParamSpecList, isPlan } from './guards';
import type {
  ChatChunk,
  ChatMessage,
  Config,
  ConfigUpdate,
  DecideResult,
  Info,
  ModelsResponse,
  ParamSpec,
  Plan,
} from './types';

/** The code a full context is reported with: the fix is a new chat, and the screen says so. */
export const CODE_CONTEXT_FULL = 'context_full';
/** The code a turn is refused with when no model is loaded: the fix is on Models. */
export const CODE_NO_MODEL = 'no_model';
/** This client's own code for an answer that is not the documented shape (never sent by a server). */
export const CODE_BAD_RESPONSE = 'bad_response';

export class ApiError extends Error {
  readonly status: number;
  /** The parsed JSON body, when there was one (a 400 on load carries the config error). */
  readonly body: unknown;
  /** The body's `code`, what the UI decides on; empty when it carried none. */
  readonly code: string;
  constructor(status: number, message: string, body: unknown) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.body = body;
    const code = (body as { code?: unknown } | null)?.code;
    this.code = typeof code === 'string' ? code : '';
  }
}

async function errorFrom(res: Response): Promise<ApiError> {
  let body: unknown = null;
  let message = `${res.status} ${res.statusText}`.trim();
  try {
    const text = await res.text();
    if (text) {
      try {
        body = JSON.parse(text);
        const e = (body as { error?: unknown }).error;
        if (typeof e === 'string' && e) message = e;
      } catch {
        message = text;
      }
    }
  } catch {
    // keep the status line
  }
  return new ApiError(res.status, message, body);
}

// A 401 means the server wants a key this page has not got (or has got wrong): the app asks for
// one. Every call reports it the same way, so no caller has to.
async function failed(res: Response): Promise<ApiError> {
  if (res.status === 401) auth.unauthorized();
  return errorFrom(res);
}

// `guard` checks the parts of the answer the UI reads; an answer that fails it is an error here,
// not an undefined later. Writes whose answer nobody reads have none.
async function request<T>(method: string, path: string, body?: unknown, guard?: (v: unknown) => v is T): Promise<T> {
  const headers: Record<string, string> = auth.headers();
  // Every write says it is JSON, with or without a body: the server refuses one that does not,
  // which is what keeps an HTML form (it cannot send this type) from calling the API.
  if (method !== 'GET') headers['Content-Type'] = 'application/json';
  const init: RequestInit = { method, headers };
  if (body !== undefined) init.body = JSON.stringify(body);
  const res = await fetch(path, init);
  if (!res.ok) throw await failed(res);
  const text = await res.text();
  let data: unknown = null;
  try {
    data = text ? JSON.parse(text) : null;
  } catch {
    throw new ApiError(res.status, `the server's answer to ${path} is not JSON`, { code: CODE_BAD_RESPONSE });
  }
  if (guard && !guard(data)) {
    throw new ApiError(res.status, `the server's answer to ${path} is not what this UI expects`, {
      code: CODE_BAD_RESPONSE,
    });
  }
  return data as T;
}

export const api = {
  info: () => request<Info>('GET', 'api/info', undefined, isInfo),
  params: () => request<ParamSpec[]>('GET', 'api/params', undefined, isParamSpecList),
  config: () => request<Config>('GET', 'api/config', undefined, isConfig),
  updateConfig: (update: ConfigUpdate) => request<Config>('PUT', 'api/config', update, isConfig),
  load: (model?: string) => request<unknown>('POST', 'api/session/load', model ? { model } : {}),
  unload: () => request<unknown>('POST', 'api/session/unload'),
  cancel: () => request<unknown>('POST', 'api/cancel'),
  models: () => request<ModelsResponse>('GET', 'api/models', undefined, isModelsResponse),
  download: (id: string) => request<unknown>('POST', 'api/models/download', { id }),
  cancelDownload: (id: string) => request<unknown>('DELETE', `api/models/download/${encodeURIComponent(id)}`),
  /** The plan in force; `measure` runs the planner's probes again instead. */
  plan: (measure = false) => request<Plan>('GET', measure ? 'api/plan?measure=1' : 'api/plan', undefined, isPlan),
  applyPlan: () => request<Config>('POST', 'api/plan/apply', undefined, isConfig),
  /** A decision: the model picks one of `choices` from a single prefill (docs/decide.md). */
  decide: (body: { prefix: string; suffix: string; choices: string[]; reuse_prefix?: boolean }) =>
    request<DecideResult>('POST', 'api/decide', body, isDecideResult),
};

export const EVENTS_URL = 'api/events';

export interface ChatRequest {
  messages: ChatMessage[];
  maxTokens?: number;
  think?: boolean;
}

/**
 * Streams one chat completion, calling `onChunk` for every parsed chunk until `[DONE]` or the
 * end of the body. Rejects with ApiError on a non-2xx status (409 while busy) and with an
 * AbortError when `signal` fires.
 */
export async function streamChat(
  req: ChatRequest,
  onChunk: (chunk: ChatChunk) => void,
  signal?: AbortSignal,
): Promise<void> {
  const body: Record<string, unknown> = { messages: req.messages, stream: true };
  if (req.maxTokens !== undefined) body.max_tokens = req.maxTokens;
  if (req.think !== undefined) body.think = req.think;

  const res = await fetch('v1/chat/completions', {
    method: 'POST',
    headers: { ...auth.headers(), 'Content-Type': 'application/json', Accept: 'text/event-stream' },
    body: JSON.stringify(body),
    signal,
  });
  if (!res.ok) throw await failed(res);
  if (!res.body) throw new ApiError(res.status, 'empty response body', null);

  const reader = res.body.getReader();
  const decoder = new TextDecoder();
  const parser = new SseParser();
  const deliver = (events: ReturnType<SseParser['push']>): boolean => {
    for (const ev of events) {
      if (isDoneSentinel(ev.data)) return true;
      const chunk = parseChunk(ev.data);
      if (chunk) onChunk(chunk);
    }
    return false;
  };

  try {
    for (;;) {
      const { value, done } = await reader.read();
      if (done) {
        deliver(parser.push(decoder.decode()));
        deliver(parser.end());
        return;
      }
      if (deliver(parser.push(decoder.decode(value, { stream: true })))) return;
    }
  } finally {
    reader.cancel().catch(() => {});
  }
}

/**
 * Holds `GET /api/events` open and delivers each event until the server closes the stream or
 * `signal` fires. `onOpen` runs once the server has answered. Rejects with ApiError on a non-2xx
 * status (401 when a key is wanted) and with the fetch error when the connection fails. Done with
 * fetch rather than EventSource, which cannot send the key.
 */
export async function openEvents(
  onOpen: () => void,
  onEvent: (ev: SseEvent) => void,
  signal: AbortSignal,
): Promise<void> {
  const res = await fetch(EVENTS_URL, {
    headers: { ...auth.headers(), Accept: 'text/event-stream' },
    signal,
  });
  if (!res.ok) throw await failed(res);
  if (!res.body) throw new ApiError(res.status, 'empty response body', null);
  onOpen();
  const reader = res.body.getReader();
  const decoder = new TextDecoder();
  const parser = new SseParser();
  try {
    for (;;) {
      const { value, done } = await reader.read();
      if (done) {
        for (const ev of parser.push(decoder.decode())) onEvent(ev);
        for (const ev of parser.end()) onEvent(ev);
        return;
      }
      for (const ev of parser.push(decoder.decode(value, { stream: true }))) onEvent(ev);
    }
  } finally {
    reader.cancel().catch(() => {});
  }
}
