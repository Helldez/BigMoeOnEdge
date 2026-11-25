import { describe, expect, it } from 'vitest';
import { graphicsState } from './graphics';
import type { BackendPack } from './agent';

const pack = (id: string, applies: boolean, installed = false): BackendPack =>
  ({ id, title: `${id} pack`, size: 100, applies, installed }) as unknown as BackendPack;
const card = (backend: string, description: string) => ({ backend, description });

describe('graphicsState', () => {
  it('offers the pack that applies and is missing', () => {
    const s = graphicsState({ registered: ['CPU'], graphics: [], packs: [pack('a', false), pack('b', true)] });
    expect(s).toEqual({ kind: 'offer', pack: pack('b', true) });
  });
  it('says an applying pack is installed', () => {
    expect(graphicsState({ registered: [], graphics: [], packs: [pack('b', true, true)] }).kind).toBe('installed');
  });
  it('offers again a pack another release left, which the engine does not load', () => {
    const stale = { ...pack('b', true, true), installed_version: '0.42.0', loads: false };
    expect(graphicsState({ registered: ['CPU'], graphics: [], packs: [stale] })).toEqual({ kind: 'offer', pack: stale });
    const fresh = { ...pack('b', true, true), installed_version: '0.49.0', loads: true };
    expect(graphicsState({ registered: ['CPU'], graphics: [], packs: [fresh] }).kind).toBe('installed');
  });
  it('names the cards in use by their own description, once each', () => {
    const b = { registered: ['X', 'Y'], graphics: [card('X', 'Card One'), card('Y', 'Card One'), card('Z', '')], packs: [] };
    expect(graphicsState(b)).toEqual({ kind: 'ready', cards: ['Card One', 'Z'] });
  });
  it('says there is no card when the server found none', () => {
    expect(graphicsState({ registered: ['CPU'], graphics: [], packs: [pack('a', false)] })).toEqual({ kind: 'ready', cards: [] });
  });
  it('says nothing when it cannot know', () => {
    expect(graphicsState({ registered: ['CPU'], graphics: [], packs: [], feed_error: 'offline' }).kind).toBe('unchecked');
    expect(graphicsState({ registered: ['CPU'], packs: [] }).kind).toBe('unknown');
  });
});
