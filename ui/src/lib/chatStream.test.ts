import { describe, expect, it } from 'vitest';
import { applyChunk, initialStreamState, parseChunk } from './chatStream';
import type { ChatChunk } from './types';

const delta = (content?: string, reasoning?: string): ChatChunk => ({
  choices: [{ delta: { content, reasoning_content: reasoning } }],
});

describe('applyChunk', () => {
  it('appends content and reasoning deltas', () => {
    let s = initialStreamState();
    s = applyChunk(s, delta(undefined, 'Let me '));
    s = applyChunk(s, delta(undefined, 'think.'));
    s = applyChunk(s, delta('Hi', undefined));
    s = applyChunk(s, delta(' there', undefined));
    expect(s.reasoning).toBe('Let me think.');
    expect(s.content).toBe('Hi there');
  });

  it('does not mutate the previous state', () => {
    const s0 = initialStreamState();
    const s1 = applyChunk(s0, delta('a'));
    expect(s0.content).toBe('');
    expect(s1.content).toBe('a');
  });

  it('replaces both fields on a reset chunk', () => {
    let s = initialStreamState();
    s = applyChunk(s, delta('<think>plan'));
    s = applyChunk(s, { ...delta('', 'plan'), bmoe: { reset: true } });
    expect(s.content).toBe('');
    expect(s.reasoning).toBe('plan');
    s = applyChunk(s, delta('Answer'));
    expect(s.content).toBe('Answer');
    expect(s.reasoning).toBe('plan');
  });

  it('treats a field missing from a reset chunk as empty', () => {
    let s = applyChunk(initialStreamState(), delta('stale', 'old'));
    s = applyChunk(s, { choices: [{ delta: { content: 'full' } }], bmoe: { reset: true } });
    expect(s.content).toBe('full');
    expect(s.reasoning).toBe('');
  });

  it('records finish reason, summary and history_dropped', () => {
    let s = applyChunk(initialStreamState(), { bmoe: { history_dropped: true } });
    const summary = { tokens: 3, tok_s: 5 } as never;
    s = applyChunk(s, { choices: [{ delta: {}, finish_reason: 'length' }], bmoe: { summary } });
    expect(s.historyDropped).toBe(true);
    expect(s.finishReason).toBe('length');
    expect(s.summary).toBe(summary);
  });

  it('records an error the server sent inside the stream', () => {
    const s = applyChunk(initialStreamState(), { error: { message: 'prompt exceeds the session n_ctx', code: 400 } });
    expect(s.error).toBe('prompt exceeds the session n_ctx');
    expect(s.content).toBe('');
    expect(applyChunk(initialStreamState(), { error: {} }).error).toBe('the generation failed');
  });

  it('keeps an earlier finish reason when later chunks carry null', () => {
    let s = applyChunk(initialStreamState(), { choices: [{ delta: {}, finish_reason: 'stop' }] });
    s = applyChunk(s, { choices: [{ delta: {}, finish_reason: null }] });
    expect(s.finishReason).toBe('stop');
  });
});

describe('parseChunk', () => {
  it('parses JSON objects and rejects everything else', () => {
    expect(parseChunk('{"choices":[]}')).toEqual({ choices: [] });
    expect(parseChunk('[DONE]')).toBeNull();
    expect(parseChunk('42')).toBeNull();
  });
});
