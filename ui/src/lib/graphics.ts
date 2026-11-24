// What the first run says about this computer's graphics, from `GET /api/backends`: the pack to
// install where one applies and is missing, else the graphics cards the engine already computes
// on, as their devices describe themselves. Pure, so each case is tested; nothing here names a
// backend or a vendor: the server says which devices are graphics cards, from their type.
import type { Backends, BackendPack } from './agent';

export type GraphicsState =
  /** A pack applies here and is not installed, or is another release's: the first run offers it as its own step. */
  | { kind: 'offer'; pack: BackendPack }
  /** The pack that applies is installed (it loads at the engine's next start). */
  | { kind: 'installed'; pack: BackendPack }
  /** Nothing to install: the cards in use by name, empty when there is none. */
  | { kind: 'ready'; cards: string[] }
  /** The list of packs could not be read, so whether one applies is not known. */
  | { kind: 'unchecked' }
  /** Nothing to say: a server that does not report its cards. */
  | { kind: 'unknown' };

/** An installed pack the engine does not use: another release left it (an older server never says). */
export function packStale(p: BackendPack): boolean {
  return p.installed && p.loads === false;
}

export function graphicsState(b: Backends): GraphicsState {
  const applies = b.packs.filter((p) => p.applies);
  // A copy another release left is as good as missing: the engine does not load it.
  const missing = applies.find((p) => !p.installed || packStale(p));
  if (missing) return { kind: 'offer', pack: missing };
  const installed = applies.find((p) => p.installed);
  if (installed) return { kind: 'installed', pack: installed };
  if (b.feed_error && b.packs.length === 0) return { kind: 'unchecked' };
  if (!b.graphics) return { kind: 'unknown' };
  const cards = [...new Set(b.graphics.map((g) => g.description || g.backend).filter(Boolean))];
  return { kind: 'ready', cards };
}
