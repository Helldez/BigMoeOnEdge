// The desktop app around this page, when there is one (lib/desktop): its version, target and
// preferences, and the notifications it shows while the window is not in front.
import { desktop, isDesktop, type DesktopInfo, type DesktopPrefs } from '../lib/desktop';
import { auth } from '../lib/auth';
import { server } from './server.svelte';

class DesktopState {
  present = $state(isDesktop());
  info = $state<DesktopInfo | null>(null);
  prefs = $state<DesktopPrefs>({});

  async start(): Promise<void> {
    this.present = isDesktop();
    if (!this.present) return;
    await this.refresh();
  }

  /** Reads the app again; a server serving the network asks its own window for the key too. */
  async refresh(): Promise<void> {
    const info = await desktop.info();
    if (!info) return;
    this.info = info;
    this.prefs = info.prefs ?? {};
    const key = info.network?.key ?? '';
    if (key && key !== auth.key()) server.submitKey(key);
  }

  async setPrefs(p: DesktopPrefs): Promise<void> {
    this.prefs = { ...this.prefs, ...p };
    await desktop.setPrefs(this.prefs);
  }

  /** Serving the network or not: the app restarts its server, and the page follows it. */
  async setServeOnNetwork(on: boolean): Promise<void> {
    await this.setPrefs({ serveOnNetwork: on });
    await this.refresh();
  }

  async newNetworkKey(): Promise<void> {
    await desktop.newNetworkKey();
    await this.refresh();
  }

  /** A desktop notification when the person is elsewhere and allowed them; nothing in a browser. */
  async notifyIfAway(title: string, body: string): Promise<void> {
    if (!this.present || this.prefs.notifications === false) return;
    if (typeof document !== 'undefined' && document.hasFocus() && document.visibilityState === 'visible') return;
    await desktop.notify(title, body);
  }
}

export const desktopState = new DesktopState();
