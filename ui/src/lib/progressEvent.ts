// Progress of a server-side transfer that is not a model download (a backend pack, an installer),
// as an event on `/api/events`: `{id?, received, total, state}`. Read loosely, since the server's
// events for these may carry fewer fields; anything else is not progress.
import { isObject } from './guards';

export interface Progress {
  id: string;
  received: number;
  total: number;
  state: string;
}

export function readProgress(data: string): Progress | null {
  let v: unknown;
  try {
    v = JSON.parse(data);
  } catch {
    return null;
  }
  if (!isObject(v) || typeof v.received !== 'number') return null;
  return {
    id: typeof v.id === 'string' ? v.id : '',
    received: v.received,
    total: typeof v.total === 'number' ? v.total : 0,
    state: typeof v.state === 'string' ? v.state : 'running',
  };
}
