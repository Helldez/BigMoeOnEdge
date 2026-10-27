// How the preview pane shows a workspace file: by the type the server gave it, with the file's
// extension as the tie-breaker (a server that answers text/plain for a .md file still gets Markdown).
export type PreviewKind = 'html' | 'svg' | 'markdown' | 'image' | 'code' | 'text' | 'binary';

const CODE_EXT = new Set(
  'c cc cpp cxx h hh hpp hxx m mm cs java kt kts swift go rs py rb php js mjs cjs ts tsx jsx svelte vue css scss less json jsonc yaml yml toml ini cfg conf sh bash zsh ps1 bat cmd sql lua pl r dart scala zig cmake gradle xml diff patch'.split(' '),
);
const IMAGE_EXT = new Set('png jpg jpeg gif webp avif bmp ico'.split(' '));
const TEXT_EXT = new Set('txt log csv tsv text'.split(' '));

export function extOf(path: string): string {
  const base = path.split('/').pop() ?? path;
  const dot = base.lastIndexOf('.');
  return dot <= 0 ? '' : base.slice(dot + 1).toLowerCase();
}

export function previewKind(path: string, contentType = ''): PreviewKind {
  const ext = extOf(path);
  const type = contentType.toLowerCase().split(';')[0].trim();
  if (type === 'image/svg+xml' || ext === 'svg') return 'svg';
  if (type === 'text/html' || ext === 'html' || ext === 'htm') return 'html';
  if (type === 'text/markdown' || ext === 'md' || ext === 'markdown') return 'markdown';
  if (type.startsWith('image/') || IMAGE_EXT.has(ext)) return 'image';
  if (CODE_EXT.has(ext)) return 'code';
  if (TEXT_EXT.has(ext) || type.startsWith('text/') || type === 'application/json' || type === '') return 'text';
  return 'binary';
}

/** The kinds the pane runs in a sandboxed frame: they may carry script, which the frame keeps away from the app. */
export const FRAMED: ReadonlySet<PreviewKind> = new Set(['html', 'svg']);

/** Text larger than this is shown cut, with the size said: a log of megabytes is not read in a pane. */
export const PREVIEW_TEXT_LIMIT = 512 * 1024;
