// Typed client for the bmoe-server HTTP API. Paths are relative so the UI works from any mount.
import { SseParser, isDoneSentinel } from './sse';
import { parseChunk } from './chatStream';
import type {
  ChatChunk,
  ChatMessage,
  Config,
  ConfigUpdate,
  Info,
  ModelsResponse,
  ParamSpec,
  Plan,
} from './types';

export class ApiError extends Error {
  readonly status: number;
  /** The parsed JSON body, when there was one (a 400 on load carries the config error). */
  readonly body: unknown;
  constructor(status: number, message: string, body: unknown) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.body = body;
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

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  const init: RequestInit = { method, headers: {} };
  if (body !== undefined) {
    init.headers = { 'Content-Type': 'application/json' };
    init.body = JSON.stringify(body);
  }
  const res = await fetch(path, init);
  if (!res.ok) throw await errorFrom(res);
  const text = await res.text();
  return (text ? JSON.parse(text) : null) as T;
}

export const api = {
  info: () => request<Info>('GET', 'api/info'),
  params: () => request<ParamSpec[]>('GET', 'api/params'),
  config: () => request<Config>('GET', 'api/config'),
  updateConfig: (update: ConfigUpdate) => request<Config>('PUT', 'api/config', update),
  load: (model?: string) => request<unknown>('POST', 'api/session/load', model ? { model } : {}),
  unload: () => request<unknown>('POST', 'api/session/unload'),
  cancel: () => request<unknown>('POST', 'api/cancel'),
  models: () => request<ModelsResponse>('GET', 'api/models'),
  download: (id: string) => request<unknown>('POST', 'api/models/download', { id }),
  cancelDownload: (id: string) => request<unknown>('DELETE', `api/models/download/${encodeURIComponent(id)}`),
  /** The plan in force; `measure` runs the planner's probes again instead. */
  plan: (measure = false) => request<Plan>('GET', measure ? 'api/plan?measure=1' : 'api/plan'),
  applyPlan: () => request<Config>('POST', 'api/plan/apply'),
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
    headers: { 'Content-Type': 'application/json', Accept: 'text/event-stream' },
    body: JSON.stringify(body),
    signal,
  });
  if (!res.ok) throw await errorFrom(res);
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
