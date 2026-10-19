import { describe, expect, it } from 'vitest';
import {
  isConfig,
  isDecideResult,
  isHfAdded,
  isHfFacts,
  isHfModelsResponse,
  isHfSearchResponse,
  isInfo,
  isModelsResponse,
  isParamSpecList,
  isPlan,
  isTokenEvent,
} from './guards';

describe('guards', () => {
  it('accept the shapes the server documents', () => {
    expect(isInfo({ version: '1', state: 'empty', generating: false, model: null })).toBe(true);
    expect(isInfo({ version: '1', state: 'ready', generating: true, model: { path: 'p', name: 'n', streaming: true } })).toBe(true);
    expect(isConfig({ values: {}, user_keys: [], valid: true })).toBe(true);
    expect(isParamSpecList([{ key: 'k', type: 'bool', group: 'g' }])).toBe(true);
    expect(isModelsResponse({ models_dir: '/m', local: [{ name: 'a', path: '/m/a' }], catalog: [] })).toBe(true);
    expect(isPlan({ available: false, reason: 'no planner' })).toBe(true);
    expect(isPlan({ available: true, error: 'no model' })).toBe(true);
    expect(isPlan({ available: true, none: true })).toBe(true);
    expect(isPlan({ available: true, decisions: [] })).toBe(true);
    expect(isDecideResult({ cancelled: false, best: 0, choice_logp: [-1, null] })).toBe(true);
    expect(isTokenEvent({ step: 1, wall_ms: 3 })).toBe(true);
    expect(isHfModelsResponse({ repo: 'a/b', models: [{ name: 'x.gguf', bytes: 1, complete: true, files: [] }] })).toBe(true);
    expect(
      isHfFacts({
        name: 'x.gguf',
        bytes: 1,
        model: { readable: true },
        verdict: { downloadable: true, mode: 'streams', disk: 'd', memory: 'm' },
      }),
    ).toBe(true);
    expect(isHfAdded({ id: 'hf:a/b/x.gguf' })).toBe(true);
    expect(isHfSearchResponse({ query: 'gemma', repos: [{ id: 'a/b' }] })).toBe(true);
  });

  it('refuse what is not one: a page of HTML, a list for an object, a missing field', () => {
    expect(isInfo('<html>')).toBe(false);
    expect(isInfo(null)).toBe(false);
    expect(isInfo({ version: '1', state: 'ready', generating: false, model: { path: 'p' } })).toBe(false);
    expect(isConfig([])).toBe(false);
    expect(isConfig({ values: [], user_keys: [], valid: true })).toBe(false);
    expect(isParamSpecList({})).toBe(false);
    expect(isParamSpecList([{ key: 'k' }])).toBe(false);
    expect(isModelsResponse({ models_dir: '/m', local: [{}], catalog: [] })).toBe(false);
    expect(isPlan({ available: true })).toBe(false);
    expect(isDecideResult({ cancelled: false, best: '0', choice_logp: [] })).toBe(false);
    expect(isTokenEvent({ step: 1 })).toBe(false);
    expect(isHfModelsResponse({ repo: 'a/b', models: [{ name: 'x.gguf' }] })).toBe(false);
    expect(isHfFacts({ name: 'x.gguf', bytes: 1, model: {}, verdict: { downloadable: true } })).toBe(false);
    expect(isHfAdded({})).toBe(false);
    expect(isHfSearchResponse({ query: 'gemma', repos: [{}] })).toBe(false);
  });
});
