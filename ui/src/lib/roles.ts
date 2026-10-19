// What a gguf file is for, as the server reports it (`role`, docs/server-api.md, "Models"): a
// model to chat with, or a companion file that only goes with one. The server decides; this only
// names the answer and keeps the companions apart so they do not read as models.

/** True for a file the server says is not a model to load: a projector, a draft head, anything else. */
export function isCompanion(role: string | undefined): boolean {
  // "" is a header that could not be read: nothing is known, so it is not set aside.
  return !!role && role !== 'model';
}

/** A person's word for a companion role. */
export function roleLabel(role: string): string {
  switch (role) {
    case 'projector':
      return 'projector';
    case 'draft':
      return 'draft head';
    case 'model':
      return 'model';
    default:
      return 'not a model';
  }
}

/** The models first, the companion files after them, each in the order given. */
export function splitByRole<T extends { role?: string }>(items: readonly T[]): { models: T[]; companions: T[] } {
  const models: T[] = [];
  const companions: T[] = [];
  for (const i of items) (isCompanion(i.role) ? companions : models).push(i);
  return { models, companions };
}

/** The date part of an ISO timestamp, "" when there is none. */
export function isoDate(ts: string | undefined): string {
  return ts && /^\d{4}-\d{2}-\d{2}/.test(ts) ? ts.slice(0, 10) : '';
}
