// Hash routes. The server serves static files only, with no SPA fallback, so the route lives
// after the '#'.

export const ROUTES = ['chat', 'models', 'plan', 'settings', 'skills', 'tasks', 'connect', 'setup'] as const;
export type Route = (typeof ROUTES)[number];

export const DEFAULT_ROUTE: Route = 'chat';

export function parseHash(hash: string): Route {
  const name = hash.replace(/^#\/?/, '').split(/[/?]/)[0];
  return (ROUTES as readonly string[]).includes(name) ? (name as Route) : DEFAULT_ROUTE;
}

export function href(route: Route): string {
  return `#/${route}`;
}

/** The setting a link points at: `#/settings/<key>` names the parameter `key`; "" when none. */
export function parseTarget(hash: string): string {
  const parts = hash.replace(/^#\/?/, '').split('?')[0].split('/');
  if (parts.length < 2 || !parts[1]) return '';
  try {
    return decodeURIComponent(parts[1]);
  } catch {
    return '';
  }
}

/** A link to the settings screen, scrolled to parameter `key` (a key the server named). */
export function settingHref(key: string): string {
  return `${href('settings')}/${encodeURIComponent(key)}`;
}
