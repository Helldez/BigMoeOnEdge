// The server's check of a model against this machine (`POST /api/models/inspect`), as a first run
// holds it: asked when a model is picked, kept for the visit, shown as the server worded it. No
// threshold and no wording of the verdict lives here: the sentence and its tone are the server's.
import type { CatalogCheck } from './types';

export type CheckState =
  | { state: 'pending' }
  | { state: 'done'; check: CatalogCheck }
  | { state: 'failed'; why: string };

/** The tone the server said its sentence in, as a class a screen colours by; '' where it claimed none. */
export function checkTone(c: CheckState | undefined): 'ok' | 'warn' | 'err' | '' {
  if (c?.state !== 'done') return '';
  const tone = c.check.verdict.tone;
  return tone === 'ok' || tone === 'warn' || tone === 'err' ? tone : '';
}

/** The sentence to show for a finished check: the plain one, or the headline from a server without it. */
export function checkSentence(c: CatalogCheck): string {
  return c.verdict.summary || c.verdict.headline || c.verdict.mode;
}

/**
 * Whether the download may start: not while the check runs (its answer is a second away), and not
 * when the server said the model cannot be downloaded here. A check that could not be made refuses
 * nothing: the download has its own checks.
 */
export function mayDownload(c: CheckState | undefined): boolean {
  if (!c || c.state === 'pending') return false;
  return c.state === 'failed' || c.check.verdict.downloadable;
}
