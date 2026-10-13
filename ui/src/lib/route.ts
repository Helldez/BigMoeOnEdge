// Hash routes. The server serves static files only, with no SPA fallback, so the route lives
// after the '#'.

export const ROUTES = ['chat', 'models', 'settings', 'plan'] as const;
export type Route = (typeof ROUTES)[number];

export const DEFAULT_ROUTE: Route = 'chat';

export function parseHash(hash: string): Route {
  const name = hash.replace(/^#\/?/, '').split(/[/?]/)[0];
  return (ROUTES as readonly string[]).includes(name) ? (name as Route) : DEFAULT_ROUTE;
}

export function href(route: Route): string {
  return `#/${route}`;
}
