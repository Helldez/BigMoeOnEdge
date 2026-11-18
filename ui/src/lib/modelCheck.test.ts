import { describe, expect, it } from 'vitest';
import { checkSentence, checkTone, mayDownload, type CheckState } from './modelCheck';
import type { CatalogCheck } from './types';

const check = (verdict: Partial<CatalogCheck['verdict']>): CatalogCheck =>
  ({
    id: 'm',
    title: 'M',
    quant: '',
    bytes: 1,
    files: 1,
    model: { readable: true },
    machine: {},
    verdict: { downloadable: true, mode: 'streams', disk: 'd', memory: 'm', ...verdict },
  }) as CatalogCheck;
const done = (verdict: Partial<CatalogCheck['verdict']>): CheckState => ({ state: 'done', check: check(verdict) });

describe('modelCheck', () => {
  it('colours by the tone the server sent, and by nothing else', () => {
    expect(checkTone(done({ tone: 'ok' }))).toBe('ok');
    expect(checkTone(done({ tone: 'err' }))).toBe('err');
    expect(checkTone(done({ tone: 'a tone this build does not know' }))).toBe('');
    expect(checkTone(done({ fit: 'fits' }))).toBe('');
    expect(checkTone({ state: 'pending' })).toBe('');
    expect(checkTone(undefined)).toBe('');
  });
  it('shows the plain sentence, or the headline of a server that has none', () => {
    expect(checkSentence(check({ summary: 'Runs here.', headline: 'Fits here' }))).toBe('Runs here.');
    expect(checkSentence(check({ headline: 'Fits here' }))).toBe('Fits here');
    expect(checkSentence(check({}))).toBe('streams');
  });
  it('holds the download while the check runs and where the server refuses it, not when the check failed', () => {
    expect(mayDownload(undefined)).toBe(false);
    expect(mayDownload({ state: 'pending' })).toBe(false);
    expect(mayDownload(done({}))).toBe(true);
    expect(mayDownload(done({ downloadable: false }))).toBe(false);
    expect(mayDownload({ state: 'failed', why: 'offline' })).toBe(true);
  });
});
