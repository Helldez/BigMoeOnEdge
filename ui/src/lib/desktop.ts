// The desktop app's bridge (docs/desktop-app.md): `window.bmoeDesktop`, present only inside the
// app's own window. Every use goes through here, so a browser (or a phone on the network) gets a
// plain "not here" instead of a TypeError, and a bridge call that throws is an answer, not a crash.

export interface DesktopPrefs {
  closeToTray?: boolean;
  port?: number;
  notifications?: boolean;
  uiScale?: number;
  serveOnNetwork?: boolean;
}

/** While the app's server serves the network: where other devices reach it, and the key they send. */
export interface DesktopNetwork {
  addresses: string[];
  key: string;
}

export interface DesktopInfo {
  version: string;
  target: string;
  prefs: DesktopPrefs;
  network?: DesktopNetwork | null;
}

/** The calls the app adds; each may answer at once or with a promise. */
export interface DesktopBridge {
  info(): DesktopInfo | Promise<DesktopInfo>;
  pickFolder(): string | null | Promise<string | null>;
  notify(title: string, body: string): unknown;
  openExternal(url: string): unknown;
  setPrefs(prefs: DesktopPrefs): unknown;
  restartServer(): unknown;
  newNetworkKey(): unknown;
  applyUpdate(path: string): unknown;
}

const CALLS: (keyof DesktopBridge)[] = ['info', 'pickFolder', 'notify', 'openExternal', 'setPrefs', 'restartServer', 'newNetworkKey', 'applyUpdate'];

/** The bridge when this page runs in the app, null in a browser. `scope` is for tests. */
export function desktopBridge(scope: unknown = globalThis): DesktopBridge | null {
  const b = (scope as { bmoeDesktop?: unknown } | null)?.bmoeDesktop;
  if (typeof b !== 'object' || b === null) return null;
  // A bridge missing a call is an older app: the calls it has still work, the others answer null.
  const has = (k: keyof DesktopBridge) => typeof (b as Record<string, unknown>)[k] === 'function';
  return CALLS.some(has) ? (b as DesktopBridge) : null;
}

export const isDesktop = (scope: unknown = globalThis): boolean => desktopBridge(scope) !== null;

async function call<T>(name: keyof DesktopBridge, args: unknown[], scope: unknown): Promise<T | null> {
  const b = desktopBridge(scope);
  const fn = b ? (b as unknown as Record<string, unknown>)[name] : undefined;
  if (typeof fn !== 'function') return null;
  try {
    const v = await (fn as (...a: unknown[]) => unknown).apply(b, args);
    return (v ?? null) as T | null;
  } catch {
    return null;
  }
}

export const desktop = {
  info: (scope: unknown = globalThis) => call<DesktopInfo>('info', [], scope),
  /** The chosen folder; null when the person cancelled or there is no app. */
  pickFolder: (scope: unknown = globalThis) => call<string>('pickFolder', [], scope),
  /** True when the app took the notification. */
  notify: async (title: string, body: string, scope: unknown = globalThis) => {
    if (!isDesktop(scope)) return false;
    await call('notify', [title, body], scope);
    return true;
  },
  /** Opens a link in the default browser; in a browser the link opens a tab by itself. */
  openExternal: (url: string, scope: unknown = globalThis) => call('openExternal', [url], scope),
  setPrefs: (prefs: DesktopPrefs, scope: unknown = globalThis) => call('setPrefs', [prefs], scope),
  /** True when the app restarts the server; false in a browser, where the person restarts it. */
  restartServer: async (scope: unknown = globalThis) => {
    if (!isDesktop(scope)) return false;
    await call('restartServer', [], scope);
    return true;
  },
  /** A new network key; the app restarts its server with it. False in a browser. */
  newNetworkKey: async (scope: unknown = globalThis) => (await call<boolean>('newNetworkKey', [], scope)) === true,
  applyUpdate: async (path: string, scope: unknown = globalThis) => {
    if (!isDesktop(scope)) return false;
    await call('applyUpdate', [path], scope);
    return true;
  },
};
