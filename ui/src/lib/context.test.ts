import { describe, expect, it } from 'vitest';
import { contextAdvice } from './context';
import { parseHash, parseTarget, settingHref } from './route';
import type { ModelInfo, ParamSpec } from './types';

const model = (n_ctx: number, ctx_advice?: ModelInfo['ctx_advice']): ModelInfo => ({
  path: 'm.gguf',
  name: 'm.gguf',
  arch: 'qwen3moe',
  n_ctx,
  n_expert_used: 8,
  think_ctl: 'template',
  load_s: 1,
  streaming: true,
  compute_threads: 8,
  busy_threads: 10,
  ctx_advice,
});

// The key is whatever the server names: these tests use a made-up one, so nothing depends on it.
const params = [{ key: 'some-knob', label: 'Window' } as ParamSpec];

describe('context advice', () => {
  it('reads the recommendation, the shortfall and the setting from the server', () => {
    const a = contextAdvice(model(4096, { recommended: 16384, short: true, param: 'some-knob' }), params);
    expect(a).toEqual({ nCtx: 4096, recommended: 16384, short: true, label: 'Window', href: '#/settings/some-knob' });
  });

  it('is not short when the server says the context fits', () => {
    expect(contextAdvice(model(32768, { recommended: 16384, short: false, param: 'some-knob' }), params)?.short).toBe(false);
  });

  it('falls back to the key before the parameter table has arrived', () => {
    expect(contextAdvice(model(4096, { recommended: 16384, short: true, param: 'some-knob' }), null)?.label).toBe('some-knob');
  });

  it('says nothing without a model or from a server that sends no advice', () => {
    expect(contextAdvice(null, params)).toBeNull();
    expect(contextAdvice(model(4096), params)).toBeNull();
  });
});

describe('setting links', () => {
  it('round-trips a parameter key through the hash', () => {
    const h = settingHref('some-knob');
    expect(parseHash(h)).toBe('settings');
    expect(parseTarget(h)).toBe('some-knob');
  });

  it('has no target on a plain route', () => {
    expect(parseTarget('#/settings')).toBe('');
    expect(parseTarget('#/settings?x=1')).toBe('');
    expect(parseTarget('')).toBe('');
    expect(parseTarget('#/settings/%E0%A4%A')).toBe('');
  });
});
