import { describe, expect, it, vi } from 'vitest';
import { desktop, desktopBridge, isDesktop } from './desktop';

describe('the desktop bridge', () => {
  it('is absent in a browser, and every call answers without throwing', async () => {
    const scope = {};
    expect(desktopBridge(scope)).toBeNull();
    expect(isDesktop(scope)).toBe(false);
    expect(await desktop.pickFolder(scope)).toBeNull();
    expect(await desktop.info(scope)).toBeNull();
    expect(await desktop.notify('t', 'b', scope)).toBe(false);
    expect(await desktop.restartServer(scope)).toBe(false);
    expect(await desktop.applyUpdate('/x', scope)).toBe(false);
    expect(await desktop.newNetworkKey(scope)).toBe(false);
  });

  it('asks the app for a new network key', async () => {
    const newNetworkKey = vi.fn(() => true);
    expect(await desktop.newNetworkKey({ bmoeDesktop: { newNetworkKey } })).toBe(true);
    expect(newNetworkKey).toHaveBeenCalled();
  });

  it('ignores a bridge that is not an object with calls', () => {
    expect(desktopBridge({ bmoeDesktop: 'yes' })).toBeNull();
    expect(desktopBridge({ bmoeDesktop: {} })).toBeNull();
    expect(desktopBridge(null)).toBeNull();
  });

  it('passes calls through, sync or async', async () => {
    const notify = vi.fn();
    const scope = { bmoeDesktop: { pickFolder: () => Promise.resolve('/home/me/p'), notify, info: () => ({ version: '1', target: 't', prefs: {} }) } };
    expect(isDesktop(scope)).toBe(true);
    expect(await desktop.pickFolder(scope)).toBe('/home/me/p');
    expect(await desktop.notify('Title', 'Body', scope)).toBe(true);
    expect(notify).toHaveBeenCalledWith('Title', 'Body');
    expect((await desktop.info(scope))?.version).toBe('1');
  });

  it('answers null for a call an older app lacks, and for one that throws', async () => {
    const scope = { bmoeDesktop: { notify: () => {}, pickFolder: () => { throw new Error('no'); } } };
    expect(await desktop.info(scope)).toBeNull();
    expect(await desktop.pickFolder(scope)).toBeNull();
  });
});
