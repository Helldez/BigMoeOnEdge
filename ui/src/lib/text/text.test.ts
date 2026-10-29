import { describe, expect, it } from 'vitest';
import { MODES } from '../agent';
import { t, tp } from './index';
import { en } from './en';

const sources = import.meta.glob('../../**/*.{svelte,ts}', { query: '?raw', import: 'default', eager: true }) as Record<string, string>;

describe('text', () => {
  it('leaves no word empty', () => {
    for (const [k, v] of Object.entries(en)) expect(v.trim(), k).not.toBe('');
  });

  it('has a word for every key the code names', () => {
    const missing = new Set<string>();
    for (const [file, text] of Object.entries(sources)) {
      if (file.includes('/text/') || file.endsWith('.test.ts')) continue;
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
      ...['chat', 'models', 'plan', 'settings', 'skills', 'tasks', 'connect', 'setup'].map((r) => `nav.${r}`),
      ...['general', 'providers', 'system', 'engine', 'metrics'].map((s) => `settings.section.${s}`),
      ...['system', 'light', 'dark'].map((s) => `settings.theme.${s}`),
      ...['bundled', 'user', 'workspace'].map((s) => `skills.source.${s}`),
      ...['once', 'interval', 'daily', 'weekly'].map((k) => `tasks.kind.${k}`),
      ...['compute', 'disk', 'mgmt', 'other'].flatMap((k) => [`metrics.split.${k}`, `metrics.split.${k}.hint`]),
      ...['read', 'edited', 'created', 'listed', 'searched', 'commands', 'pages', 'skills', 'other'].flatMap((k) =>
        ['first', 'next'].flatMap((p) => ['one', 'other'].map((f) => `run.${k}.${p}.${f}`)),
      ),
    ];
    expect(need.filter((k) => !(k in en))).toEqual([]);
  });

  it('fills placeholders and picks plurals', () => {
    expect(t('models.loadTime', { s: '1.5' })).toBe('loaded in 1.5 s');
    expect(tp('providers.models', 1)).toBe('1 model');
    expect(tp('providers.models', 3)).toBe('3 models');
    expect(t('no.such.key')).toBe('no.such.key');
  });
});
