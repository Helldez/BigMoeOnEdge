// Folds streamed chat chunks into the assistant message being built. Pure: returns a new state.
import type { ChatChunk, DoneSummary } from './types';

export interface StreamState {
  content: string;
  reasoning: string;
  finishReason: string | null;
  summary: DoneSummary | null;
  historyDropped: boolean;
  /** Set when the server reported a failure inside the stream (the status was already 200). */
  error: string;
}

export function initialStreamState(): StreamState {
  return { content: '', reasoning: '', finishReason: null, summary: null, historyDropped: false, error: '' };
}

/**
 * Applies one chunk. Normal chunks append their delta. A chunk with `bmoe.reset` carries the
 * FULL text so far, so it replaces both fields; a field it omits is taken as empty, because the
 * reset describes the complete classification of everything sent until now.
 */
export function applyChunk(state: StreamState, chunk: ChatChunk): StreamState {
  const next: StreamState = { ...state };
  const choice = chunk.choices?.[0];
  const delta = choice?.delta ?? {};
  const content = delta.content ?? '';
  const reasoning = delta.reasoning_content ?? '';

  if (chunk.bmoe?.reset) {
    next.content = content;
    next.reasoning = reasoning;
  } else {
    next.content += content;
    next.reasoning += reasoning;
  }
  if (choice?.finish_reason) next.finishReason = choice.finish_reason;
  if (chunk.bmoe?.summary) next.summary = chunk.bmoe.summary;
  if (chunk.bmoe?.history_dropped) next.historyDropped = true;
  if (chunk.error) next.error = chunk.error.message || 'the generation failed';
  return next;
}

/** Parses the JSON of one `data:` payload; null when it is not a JSON object. */
export function parseChunk(data: string): ChatChunk | null {
  try {
    const v: unknown = JSON.parse(data);
    return v && typeof v === 'object' ? (v as ChatChunk) : null;
  } catch {
    return null;
  }
}
