import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { chat } from './chat.svelte';

// The chat store's failure path, against a stubbed server: what stays in the transcript, what the
// screen is told to offer, and that the message can be sent again (docs/screens.md, Errors).
type Reply = { status: number; body: string; type?: string };

const sse = (...chunks: unknown[]) => chunks.map((c) => `data: ${JSON.stringify(c)}\n\n`).join('') + 'data: [DONE]\n\n';
const answer = (text: string) => sse({ choices: [{ index: 0, delta: { content: text }, finish_reason: 'stop' }] });

let replies: Reply[] = [];
const sent: unknown[] = [];

function serve(...r: Reply[]): void {
  replies = r;
}

beforeEach(() => {
  sent.length = 0;
  vi.stubGlobal('fetch', async (path: string, init: RequestInit) => {
    if (path === 'v1/chat/completions' || path === 'api/decide') sent.push(JSON.parse(String(init.body)));
    const r = replies.shift() ?? { status: 500, body: '{"error":"no reply stubbed"}' };
    return new Response(r.body, { status: r.status, headers: { 'Content-Type': r.type ?? 'application/json' } });
  });
  chat.newChat();
});

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('a send that fails before anything streams', () => {
  it('keeps the message in the transcript, with the code the screen decides on', async () => {
    serve({ status: 503, body: '{"error":"no model is loaded","code":"no_model"}' });
    await chat.send('hello');
    expect(chat.turns.map((t) => `${t.role}:${t.content}`)).toEqual(['user:hello']);
    expect(chat.error).toBe('no model is loaded');
    expect(chat.errorCode).toBe('no_model');
    expect(chat.noModel).toBe(true);
    expect(chat.contextFull).toBe(false);
    expect(chat.retryable).toBe(true);
    expect(chat.busy).toBe(false);
  });

  it('sends the same conversation again on retry, and clears the error once answered', async () => {
    serve({ status: 503, body: '{"error":"no model is loaded","code":"no_model"}' });
    await chat.send('hello');
    serve({ status: 200, body: answer('hi there'), type: 'text/event-stream' });
    await chat.retry();
    expect(sent).toHaveLength(2);
    expect((sent[1] as { messages: unknown[] }).messages).toEqual([{ role: 'user', content: 'hello' }]);
    expect(chat.turns.map((t) => `${t.role}:${t.content}`)).toEqual(['user:hello', 'assistant:hi there']);
    expect(chat.error).toBe('');
    expect(chat.errorCode).toBe('');
    expect(chat.retryable).toBe(false);
  });

  it('reads a failure inside an answered stream the same way: a full context offers a new chat', async () => {
    serve({ status: 200, body: sse({ error: { message: 'any wording', code: 'context_full' } }), type: 'text/event-stream' });
    await chat.send('a long one');
    expect(chat.turns.map((t) => t.role)).toEqual(['user']);
    expect(chat.contextFull).toBe(true);
    expect(chat.error).toBe('any wording');
  });

  it('decides on the code, never on the wording', async () => {
    serve({ status: 503, body: '{"error":"Load a model first","code":"busy"}' });
    await chat.send('x');
    expect(chat.noModel).toBe(false);
    expect(chat.errorCode).toBe('busy');
  });
});

describe('a turn that fails part way', () => {
  it('keeps what streamed as the answer, with the error under it', async () => {
    serve({
      status: 200,
      body: sse({ choices: [{ index: 0, delta: { content: 'half an' } }] }, { error: { message: 'decode failed', code: 'failed' } }),
      type: 'text/event-stream',
    });
    await chat.send('q');
    expect(chat.turns.map((t) => `${t.role}:${t.content}`)).toEqual(['user:q', 'assistant:half an']);
    expect(chat.turns[1].error).toBe('decode failed');
    expect(chat.error).toBe('');
    expect(chat.retryable).toBe(false);
  });
});

describe('a decision that fails', () => {
  it('leaves no question behind to be sent as history', async () => {
    serve({ status: 400, body: '{"error":"two choices share a first token","code":"invalid_request"}' });
    await chat.decide('which?', 'A\nB');
    expect(chat.turns).toEqual([]);
    expect(chat.error).toBe('two choices share a first token');
    expect(chat.retryable).toBe(false);
  });
});
