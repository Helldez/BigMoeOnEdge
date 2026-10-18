import { afterEach, describe, expect, it, vi } from 'vitest';
import { api, openEvents, streamChat } from './api';
import { auth } from './auth';
import type { SseEvent } from './sse';

// The server refuses a write that does not declare JSON, so the client must on every one.
describe('api', () => {
  const calls: { path: string; init: RequestInit }[] = [];
  // Each read answers the least of its documented shape, which the client checks before it returns.
  const shapes: Record<string, string> = {
    'api/info': '{"version":"1","state":"empty","generating":false,"model":null}',
    'api/config': '{"values":{},"user_keys":[],"valid":true}',
    'api/plan/apply': '{"values":{},"user_keys":[],"valid":true}',
  };
  const stub = () =>
    vi.stubGlobal('fetch', async (path: string, init: RequestInit) => {
      calls.push({ path, init });
      return new Response(shapes[path] ?? '{}', { status: 200 });
    });
  const contentType = (init: RequestInit) => new Headers(init.headers).get('Content-Type');

  afterEach(() => {
    calls.length = 0;
    vi.unstubAllGlobals();
  });

  it('declares JSON on every write, with or without a body', async () => {
    stub();
    await api.unload();
    await api.cancel();
    await api.applyPlan();
    await api.cancelDownload('a/b');
    await api.load('m.gguf');
    await api.download('id');
    await api.updateConfig({ values: {} });
    await streamChat({ messages: [{ role: 'user', content: 'hi' }] }, () => {});
    expect(calls.map((c) => `${c.init.method} ${c.path}`)).toEqual([
      'POST api/session/unload',
      'POST api/cancel',
      'POST api/plan/apply',
      'DELETE api/models/download/a%2Fb',
      'POST api/session/load',
      'POST api/models/download',
      'PUT api/config',
      'POST v1/chat/completions',
    ]);
    for (const c of calls) expect(contentType(c.init), c.path).toBe('application/json');
    expect(calls[0].init.body).toBeUndefined();
    expect(calls[4].init.body).toBe('{"model":"m.gguf"}');
  });

  it('sends a read as a plain GET', async () => {
    stub();
    await api.info();
    expect(calls[0].init.method).toBe('GET');
    expect(contentType(calls[0].init)).toBeNull();
  });
  it('sends the key as a bearer token on every request once one is set, and not before', async () => {
    stub();
    auth.set('');
    await api.info();
    expect(new Headers(calls[0].init.headers).get('Authorization')).toBeNull();
    calls.length = 0;
    auth.set('  s3cret ');
    try {
      await api.info();
      await api.cancel();
      await streamChat({ messages: [{ role: 'user', content: 'hi' }] }, () => {});
      const ac = new AbortController();
      await openEvents(() => {}, () => {}, ac.signal);
      expect(calls).toHaveLength(4);
      for (const c of calls) expect(new Headers(c.init.headers).get('Authorization'), c.path).toBe('Bearer s3cret');
      expect(contentType(calls[1].init)).toBe('application/json');
    } finally {
      auth.set('');
    }
  });

  it('reports a 401 to whoever asked to hear of one, and still rejects the call', async () => {
    vi.stubGlobal('fetch', async () => new Response('{"error":"an API key is required"}', { status: 401 }));
    let told = 0;
    const off = auth.onUnauthorized(() => told++);
    try {
      await expect(api.info()).rejects.toMatchObject({ status: 401, message: 'an API key is required' });
      const ac = new AbortController();
      await expect(openEvents(() => {}, () => {}, ac.signal)).rejects.toMatchObject({ status: 401 });
      expect(told).toBe(2);
    } finally {
      off();
    }
  });

  it('carries the code of an error beside its message, and none when the body has none', async () => {
    vi.stubGlobal('fetch', async () => new Response('{"error":"any wording","code":"context_full"}', { status: 400 }));
    await expect(api.info()).rejects.toMatchObject({ status: 400, message: 'any wording', code: 'context_full' });
    vi.stubGlobal('fetch', async () => new Response('{"error":"no code"}', { status: 400 }));
    await expect(api.info()).rejects.toMatchObject({ code: '' });
  });

  it('refuses an answer that is not the documented shape, with a code of its own', async () => {
    vi.stubGlobal('fetch', async () => new Response('{"something":"else"}', { status: 200 }));
    await expect(api.info()).rejects.toMatchObject({ code: 'bad_response' });
    await expect(api.config()).rejects.toMatchObject({ code: 'bad_response' });
    vi.stubGlobal('fetch', async () => new Response('<html>a proxy</html>', { status: 200 }));
    await expect(api.models()).rejects.toMatchObject({ code: 'bad_response' });
    vi.stubGlobal('fetch', async () => new Response('', { status: 200 }));
    await expect(api.unload()).resolves.toBeNull();
  });

  it('delivers the events of the stream by name until it ends', async () => {
    const body = 'event: state\ndata: {"state":"empty"}\n\nevent: token\ndata: {"wall_ms":3}\n\n';
    vi.stubGlobal('fetch', async () => new Response(body, { status: 200 }));
    const seen: SseEvent[] = [];
    let opened = 0;
    const ac = new AbortController();
    await openEvents(() => opened++, (ev) => seen.push(ev), ac.signal);
    expect(opened).toBe(1);
    expect(seen.map((e) => `${e.event} ${e.data}`)).toEqual(['state {"state":"empty"}', 'token {"wall_ms":3}']);
  });
});
