import { describe, expect, it } from 'vitest';
import { downloadSummary, fitTone, libraryRows, recommendedEntry } from './library';
import type { CatalogEntry, DownloadEvent, LocalModel } from './types';

// Made-up names and sizes: nothing here depends on a real model.
const DIR = '/data/models';

const local = (name: string, extra: Partial<LocalModel> = {}): LocalModel => ({
  name,
  path: `${DIR}/${name}`,
  dir: DIR,
  folder: '',
  role: 'model',
  files: [`${DIR}/${name}`],
  bytes: 100,
  shards: 1,
  arch: 'some-arch',
  streamable: true,
  complete: true,
  ...extra,
});

const entry = (id: string, file: string, extra: Partial<CatalogEntry> = {}): CatalogEntry => ({
  id,
  title: `Title ${id}`,
  quant: '',
  file,
  bytes: 100,
  blurb: '',
  status: 'available',
  note: '',
  ...extra,
});

const dl = (id: string, state: string, received = 10, total = 100): DownloadEvent => ({
  id,
  file: 'f',
  received,
  total,
  state,
  error: '',
});

describe('the merged models list', () => {
  it('folds a catalog entry into the file its download made, with the entry title', () => {
    const rows = libraryRows(
      { models_dir: DIR, catalog: [entry('a', 'a.gguf', { status: 'on_disk' })] },
      [local('a.gguf')],
      {},
      null,
    );
    expect(rows).toHaveLength(1);
    expect(rows[0]).toMatchObject({ state: 'on_disk', title: 'Title a', deleteName: 'a.gguf', suggested: false });
    expect(rows[0].entry?.id).toBe('a');
  });

  it('gives each row one state: loaded, downloading, partial, available', () => {
    const rows = libraryRows(
      {
        models_dir: DIR,
        catalog: [entry('d', 'd.gguf'), entry('p', 'p.gguf', { partial: true, received: 40 }), entry('s', 's.gguf')],
      },
      [local('x.gguf')],
      { d: dl('d', 'running') },
      `${DIR}/x.gguf`,
    );
    expect(rows.map((r) => [r.key, r.state, r.suggested])).toEqual([
      [`${DIR}/x.gguf`, 'loaded', false],
      ['catalog:d', 'downloading', false],
      ['catalog:p', 'partial', false],
      ['catalog:s', 'available', true],
    ]);
  });

  it('lists the suggestions last, and an added entry is not one', () => {
    const rows = libraryRows(
      { models_dir: DIR, catalog: [entry('s', 's.gguf'), entry('u', 'u.gguf', { added: true })] },
      [],
      {},
      null,
    );
    expect(rows.map((r) => r.key)).toEqual(['catalog:u', 'catalog:s']);
    expect(rows[0].suggested).toBe(false);
  });

  it('reads a finished download from its event before the listing catches up', () => {
    const rows = libraryRows({ models_dir: DIR, catalog: [entry('d', 'd.gguf', { status: 'downloading' })] }, [], { d: dl('d', 'cancelled') }, null);
    expect(rows[0].state).toBe('available');
  });

  it('keeps a split set downloading on its row while its first shard is already here', () => {
    const rows = libraryRows(
      { models_dir: DIR, catalog: [entry('b', 'b-00001-of-00002.gguf', { status: 'downloading' })] },
      [local('b-00001-of-00002.gguf', { complete: false, shards: 2 })],
      {},
      null,
    );
    expect(rows).toHaveLength(1);
    expect(rows[0].state).toBe('downloading');
  });

  it('marks a set with shards missing incomplete, and only the models folder deletable', () => {
    const rows = libraryRows(
      { models_dir: DIR, catalog: [] },
      [
        local('c-00001-of-00002.gguf', { complete: false, folder: 'c', path: `${DIR}/c/c-00001-of-00002.gguf` }),
        local('elsewhere.gguf', { dir: '/sdcard/Download', path: '/sdcard/Download/elsewhere.gguf' }),
      ],
      {},
      null,
    );
    expect(rows[0]).toMatchObject({ state: 'incomplete', deleteName: 'c/c-00001-of-00002.gguf' });
    expect(rows[1].deleteName).toBeNull();
  });

  it('never matches a file in a subfolder or a scan directory to a catalog entry', () => {
    const rows = libraryRows(
      { models_dir: DIR, catalog: [entry('a', 'a.gguf')] },
      [local('a.gguf', { folder: 'sub', path: `${DIR}/sub/a.gguf` })],
      {},
      null,
    );
    expect(rows).toHaveLength(2);
    expect(rows[0].entry).toBeNull();
  });
});

describe('downloads in the status line', () => {
  it('is nothing when nothing runs', () => {
    expect(downloadSummary({})).toBeNull();
    expect(downloadSummary({ a: dl('a', 'done') })).toBeNull();
  });

  it('counts the running ones and sums what is known', () => {
    const s = downloadSummary({ a: dl('a', 'running', 30, 100), b: dl('b', 'running', 20, 100), c: dl('c', 'error') });
    expect(s).toEqual({ count: 2, received: 50, total: 200, fraction: 0.25 });
  });

  it('has no fraction when no size is known', () => {
    expect(downloadSummary({ a: dl('a', 'running', 30, 0) })?.fraction).toBeNull();
  });
});

describe('the verdict colour', () => {
  it('follows the code, and an unknown code has none', () => {
    expect(fitTone('fits')).toBe('ok');
    expect(fitTone('no_streaming')).toBe('warn');
    expect(fitTone('will_not_fit')).toBe('err');
    expect(fitTone('something new')).toBe('');
    expect(fitTone(undefined)).toBe('');
  });
});

describe('recommendedEntry', () => {
  const e = (id: string, bytes: number, added = false) =>
    ({ id, title: id, quant: '', file: `${id}.gguf`, bytes, blurb: '', status: 'available', note: '', added }) as CatalogEntry;
  const GB = 1024 ** 3;
  it('picks the largest model that fits the memory', () => {
    expect(recommendedEntry([e('small', 14 * GB), e('big', 20 * GB)], 32 * GB)?.id).toBe('big');
    expect(recommendedEntry([e('small', 14 * GB), e('big', 20 * GB)], 16 * GB)?.id).toBe('small');
  });
  it('picks the smallest when none fits, and never one the person added', () => {
    expect(recommendedEntry([e('small', 14 * GB), e('big', 20 * GB)], 8 * GB)?.id).toBe('small');
    expect(recommendedEntry([e('mine', 1 * GB, true), e('big', 20 * GB)], 8 * GB)?.id).toBe('big');
    expect(recommendedEntry([], 8 * GB)).toBeNull();
  });
});
