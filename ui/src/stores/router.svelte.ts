// Tiny hash router: the current route follows `location.hash`.
import { parseHash, type Route } from '../lib/route';

class RouterStore {
  route = $state<Route>(parseHash(typeof location === 'undefined' ? '' : location.hash));

  start(): void {
    window.addEventListener('hashchange', () => {
      this.route = parseHash(location.hash);
    });
  }
}

export const router = new RouterStore();
