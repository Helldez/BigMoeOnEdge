import { describe, expect, it } from 'vitest';
import { isConfig, isDecideResult, isInfo, isModelsResponse, isParamSpecList, isPlan, isTokenEvent } from './guards';

describe('guards', () => {
  it('accept the shapes the server documents', () => {
    expect(isInfo({ version: '1', state: 'empty', generating: false, model: null })).toBe(true);
    expect(isInfo({ version: '1', state: 'ready', generating: true, model: { path: 'p', name: 'n', streaming: true } })).toBe(true);
    expect(isConfig({ values: {}, user_keys: [], valid: true })).toBe(true);
    expect(isParamSpecList([{ key: 'k', type: 'bool', group: 'g' }])).toBe(true);
    expect(isModelsResponse({ models_dir: '/m', local: [{ name: 'a', path: '/m/a' }], catalog: [] })).toBe(true);
    expect(isPlan({ available: false, reason: 'no planner' })).toBe(true);
    expect(isPlan({ available: true, error: 'no model' })).toBe(true);
    expect(isPlan({ available: true, decisions: [] })).toBe(true);
    expect(isDecideResult({ cancelled: false, best: 0, choice_logp: [-1, null] })).toBe(true);
    expect(isTokenEvent({ step: 1, wall_ms: 3 })).toBe(true);
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
  });
});
