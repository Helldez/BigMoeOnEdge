// The Models screen's one list (docs/screens.md, "Models"): the files on this machine and the
// catalog's entries, merged by file, each row in one state with one primary action. Everything a
// row says comes from `GET /api/models` and the `download` events: no model, size or architecture
// is named here, and what a row may do is decided by the state the server reported.
import type { CatalogEntry, DownloadEvent, LocalModel, ModelsResponse } from './types';

export type RowState =
  /** In the catalog, nothing of it here yet: Download. */
  | 'available'
  /** Its download runs: progress and Cancel. */
  | 'downloading'
  /** A stopped download left partial files: Resume, or Delete partial. */
  | 'partial'
  /** On this machine, ready to load. */
  | 'on_disk'
  /** The model the engine has open: Reload, Unload. */
  | 'loaded'
  /** A split set with shards missing: not loadable; the download (when it has an entry) fetches the rest. */
  | 'incomplete';

export interface LibraryRow {
  /** Stable across refreshes: the local path, or the catalog id for an entry with nothing here. */
  key: string;
  title: string;
  local: LocalModel | null;
  entry: CatalogEntry | null;
  state: RowState;
  /** The catalog's suggestion, nothing of it here and nothing started: listed after the others. */
  suggested: boolean;
  /** The listing's own name for the model (`folder/name`), for a delete; null when the server
   *  would not delete it (a scan directory's model, loaded in place, or nothing on disk). */
  deleteName: string | null;
}

/** A running download, as the events report it or, before the first event, as the listing does. */
export function isDownloading(entry: CatalogEntry, dl: DownloadEvent | undefined): boolean {
  return dl ? dl.state === 'running' : entry.status === 'downloading';
}

function entryState(entry: CatalogEntry, dl: DownloadEvent | undefined): RowState {
  if (isDownloading(entry, dl)) return 'downloading';
  if (entry.status === 'on_disk') return 'on_disk';
  return entry.partial ? 'partial' : 'available';
}

/** The local model a catalog entry's download made: its first file, at the top of the models folder. */
function matchesEntry(m: LocalModel, entry: CatalogEntry, modelsDir: string): boolean {
  return m.dir === modelsDir && !m.folder && m.name === entry.file;
}

/**
 * The merged list: one row per model on this machine (catalog entries that made it folded in),
 * then the entries whose download has begun, then the suggestions. Companion files (a projector,
 * a draft head) are not rows: pass `local` already split (lib/roles.ts).
 */
export function libraryRows(
  data: Pick<ModelsResponse, 'models_dir' | 'catalog'>,
  local: readonly LocalModel[],
  downloads: Readonly<Record<string, DownloadEvent>>,
  loadedPath: string | null,
): LibraryRow[] {
  const rows: LibraryRow[] = [];
  const claimed = new Set<string>();
  for (const m of local) {
    const entry = data.catalog.find((e) => !claimed.has(e.id) && matchesEntry(m, e, data.models_dir)) ?? null;
    if (entry) claimed.add(entry.id);
    const dl = entry ? downloads[entry.id] : undefined;
    let state: RowState;
    if (m.path === loadedPath) state = 'loaded';
    else if (entry && isDownloading(entry, dl)) state = 'downloading';
    else if (!m.complete) state = entry?.partial ? 'partial' : 'incomplete';
    else state = 'on_disk';
    rows.push({
      key: m.path,
      title: entry?.title ?? m.name,
      local: m,
      entry,
      state,
      suggested: false,
      deleteName: m.dir === data.models_dir ? (m.folder ? `${m.folder}/${m.name}` : m.name) : null,
    });
  }
  const started: LibraryRow[] = [];
  const suggested: LibraryRow[] = [];
  for (const e of data.catalog) {
    if (claimed.has(e.id)) continue;
    const state = entryState(e, downloads[e.id]);
    const row: LibraryRow = {
      key: `catalog:${e.id}`,
      title: e.title,
      local: null,
      entry: e,
      state,
      suggested: state === 'available' && !e.added,
      deleteName: null,
    };
    (row.suggested ? suggested : started).push(row);
  }
  return [...rows, ...started, ...suggested];
}

export interface DownloadSummary {
  count: number;
  received: number;
  total: number;
  /** Over every running download whose size is known, in [0, 1]; null when none is. */
  fraction: number | null;
}

/** The running downloads at a glance, for the status line; null when none runs. */
export function downloadSummary(downloads: Readonly<Record<string, DownloadEvent>>): DownloadSummary | null {
  let count = 0;
  let received = 0;
  let total = 0;
  for (const d of Object.values(downloads)) {
    if (d.state !== 'running') continue;
    count++;
    if (d.total > 0) {
      received += Math.min(d.received, d.total);
      total += d.total;
    }
  }
  if (count === 0) return null;
  return { count, received, total, fraction: total > 0 ? received / total : null };
}

/** The verdict's colour, by the code the server sent (`verdict.fit`); the words are the server's. */
export function fitTone(fit: string | undefined): 'ok' | 'warn' | 'err' | '' {
  switch (fit) {
    case 'fits':
      return 'ok';
    case 'no_streaming':
      return 'warn';
    case 'will_not_fit':
    case 'no_disk':
    case 'not_a_model':
      return 'err';
    default:
      return '';
  }
}
