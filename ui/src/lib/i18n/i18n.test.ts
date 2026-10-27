import { describe, expect, it } from 'vitest';
import { MODES } from '../agent';
import { LANGUAGES, pickLanguage, plural, translate } from './index';
import { en } from './en';

const sources = import.meta.glob('../../**/*.{svelte,ts}', { query: '?raw', import: 'default', eager: true }) as Record<string, string>;
const placeholders = (s: string) => [...s.matchAll(/\{(\w+)\}/g)].map((m) => m[1]).sort();

describe('i18n', () => {
  const keys = Object.keys(en).sort();

  for (const [lang, dict] of Object.entries(LANGUAGES)) {
    it(`${lang} has every key, and only those`, () => {
      expect(Object.keys(dict).sort()).toEqual(keys);
    });
    it(`${lang} keeps every placeholder and leaves nothing empty`, () => {
      for (const k of keys) {
        const v = (dict as Record<string, string>)[k];
        expect(v.trim(), k).not.toBe('');
        expect(placeholders(v), k).toEqual(placeholders((en as Record<string, string>)[k]));
      }
    });
  }

  it('has a word for every key the code names', () => {
    const missing = new Set<string>();
    for (const [file, text] of Object.entries(sources)) {
      if (file.includes('/i18n/') || file.endsWith('.test.ts')) continue;
      for (const m of text.matchAll(/\bt\('([a-zA-Z0-9_.]+)'/g)) if (!(m[1] in en)) missing.add(`${m[1]} (${file})`);
      for (const m of text.matchAll(/\btp\('([a-zA-Z0-9_.]+)'/g))
        for (const f of ['one', 'other']) if (!(`${m[1]}.${f}` in en)) missing.add(`${m[1]}.${f} (${file})`);
    }
    expect([...missing]).toEqual([]);
  });

  it('has the families the code builds keys from', () => {
    const need = [
      ...MODES.flatMap((m) => [`mode.${m}`, `mode.${m}.desc`, `tasks.mode.${m}`]),
      ...['running', 'waiting', 'done', 'denied', 'error'].map((s) => `tool.state.${s}`),
      ...['today', 'yesterday', 'week', 'month', 'older'].map((b) => `date.${b}`),
      ...['empty', 'planning', 'loading', 'error'].map((s) => `status.${s}`),
      ...['chat', 'models', 'plan', 'settings', 'metrics', 'skills', 'tasks', 'connect', 'setup'].map((r) => `nav.${r}`),
      ...['basic', 'advanced', 'experimental', 'debug'].map((l) => `settings.level.${l}`),
      ...['general', 'providers', 'system', 'engine'].map((s) => `settings.section.${s}`),
      ...['system', 'light', 'dark'].map((s) => `settings.theme.${s}`),
      ...['bundled', 'user', 'workspace'].map((s) => `skills.source.${s}`),
      ...['once', 'interval', 'daily', 'weekly'].map((k) => `tasks.kind.${k}`),
      ...['compute', 'disk', 'mgmt', 'other'].flatMap((k) => [`metrics.split.${k}`, `metrics.split.${k}.hint`]),
      ...['read', 'edited', 'created', 'listed', 'searched', 'commands', 'pages', 'web', 'skills', 'helpers', 'other'].flatMap((k) =>
        ['first', 'next'].flatMap((p) => ['one', 'other'].map((f) => `run.${k}.${p}.${f}`)),
      ),
    ];
    expect(need.filter((k) => !(k in en))).toEqual([]);
  });

  it('picks the browser language when there is one, English otherwise', () => {
    expect(pickLanguage(['it-IT', 'en'])).toBe('it');
    expect(pickLanguage(['de-DE', 'en-GB'])).toBe('en');
    expect(pickLanguage([])).toBe('en');
  });

  it('fills placeholders and picks plurals', () => {
    expect(translate('en', 'models.loadTime', { s: '1.5' })).toBe('loaded in 1.5 s');
    expect(plural('it', 'providers.models', 1)).toBe('1 modello');
    expect(plural('it', 'providers.models', 3)).toBe('3 modelli');
    expect(translate('en', 'no.such.key')).toBe('no.such.key');
  });
});
