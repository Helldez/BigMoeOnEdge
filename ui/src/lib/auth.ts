// The API key, when the server was started with one (`--api-key`). Kept in this browser and sent
// as a bearer token on every request to the API; a 401 is how the app learns it needs one.
const STORAGE_KEY = 'bmoe.apiKey';

let key = '';
try {
  key = localStorage.getItem(STORAGE_KEY) ?? '';
} catch {
  // no storage (a private window, a blocked origin): the key lasts as long as the page
}

const listeners = new Set<() => void>();

export const auth = {
  key(): string {
    return key;
  },
  set(k: string): void {
    key = k.trim();
    try {
      if (key) localStorage.setItem(STORAGE_KEY, key);
      else localStorage.removeItem(STORAGE_KEY);
    } catch {
      // kept in memory only
    }
  },
  /** What to add to a request's headers: the bearer token, when a key is known. */
  headers(): Record<string, string> {
    return key ? { Authorization: `Bearer ${key}` } : {};
  },
  /** Runs `fn` on every 401 the client sees. Returns the unsubscribe. */
  onUnauthorized(fn: () => void): () => void {
    listeners.add(fn);
    return () => {
      listeners.delete(fn);
    };
  },
  unauthorized(): void {
    for (const fn of listeners) fn();
  },
};
