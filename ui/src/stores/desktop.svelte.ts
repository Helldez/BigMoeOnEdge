// The desktop app around this page, when there is one (lib/desktop): its version, target and
// preferences, and the notifications it shows while the window is not in front.
import { desktop, isDesktop, type DesktopInfo, type DesktopPrefs } from '../lib/desktop';

class DesktopState {
  present = $state(isDesktop());
  info = $state<DesktopInfo | null>(null);
  prefs = $state<DesktopPrefs>({});

  async start(): Promise<void> {
    this.present = isDesktop();
    if (!this.present) return;
    const info = await desktop.info();
    if (info) {
      this.info = info;
      this.prefs = info.prefs ?? {};
    }
  }

  async setPrefs(p: DesktopPrefs): Promise<void> {
    this.prefs = { ...this.prefs, ...p };
    await desktop.setPrefs(this.prefs);
  }

  /** A desktop notification when the person is elsewhere and allowed them; nothing in a browser. */
  async notifyIfAway(title: string, body: string): Promise<void> {
    if (!this.present || this.prefs.notifications === false) return;
    if (typeof document !== 'undefined' && document.hasFocus() && document.visibilityState === 'visible') return;
    await desktop.notify(title, body);
  }
}

export const desktopState = new DesktopState();
