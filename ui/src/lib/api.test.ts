import { afterEach, describe, expect, it, vi } from 'vitest';
import { api, streamChat } from './api';

// The server refuses a write that does not declare JSON, so the client must on every one.
describe('api', () => {
  const calls: { path: string; init: RequestInit }[] = [];
  const stub = () =>
    vi.stubGlobal('fetch', async (path: string, init: RequestInit) => {
      calls.push({ path, init });
      return new Response('{}', { status: 200 });
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
});
