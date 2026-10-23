// Tiny hash router: the current route follows `location.hash`.
import { parseHash, parseTarget, type Route } from '../lib/route';

const currentHash = () => (typeof location === 'undefined' ? '' : location.hash);

class RouterStore {
  route = $state<Route>(parseHash(currentHash()));
  /** The setting a link pointed at (`#/settings/<key>`), "" when none. */
  target = $state(parseTarget(currentHash()));

  start(): void {
    window.addEventListener('hashchange', () => {
      this.route = parseHash(location.hash);
      this.target = parseTarget(location.hash);
    });
  }
}

export const router = new RouterStore();
