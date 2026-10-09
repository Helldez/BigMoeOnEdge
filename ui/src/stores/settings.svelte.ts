// Settings form state: the level filter (remembered per browser), debounced config writes and
// the per-field rejection reasons from the last response.
import { api } from '../lib/api';
import { OPTIONAL_LEVELS } from '../lib/schema';
import type { ParamValue } from '../lib/types';
import { server } from './server.svelte';

const LEVELS_KEY = 'bmoe.settings.levels';
const DEBOUNCE_MS = 400;

function loadLevels(): string[] {
  try {
    const raw = localStorage.getItem(LEVELS_KEY);
    const v: unknown = raw ? JSON.parse(raw) : [];
    return Array.isArray(v) ? v.filter((x): x is string => (OPTIONAL_LEVELS as readonly string[]).includes(x)) : [];
  } catch {
    return [];
  }
}

class SettingsStore {
  levels = $state<string[]>(loadLevels());
  rejected = $state<Record<string, string>>({});
  saving = $state(false);
  requestError = $state('');

  private pending: Record<string, ParamValue> = {};
  private timer: ReturnType<typeof setTimeout> | null = null;

  toggleLevel(level: string, on: boolean): void {
    const set = new Set(this.levels);
    if (on) set.add(level);
    else set.delete(level);
    this.levels = OPTIONAL_LEVELS.filter((l) => set.has(l));
    try {
      localStorage.setItem(LEVELS_KEY, JSON.stringify(this.levels));
    } catch {
      // storage unavailable: the filter just is not remembered
    }
  }

  /** Queues one value; writes are batched and sent after the user pauses typing. */
  set(key: string, value: ParamValue): void {
    this.pending[key] = value;
    if (this.timer) clearTimeout(this.timer);
    this.timer = setTimeout(() => this.flush(), DEBOUNCE_MS);
  }

  async reset(key: string): Promise<void> {
    delete this.pending[key];
    await this.send({ reset: [key] }, [key]);
  }

  async flush(): Promise<void> {
    if (this.timer) {
      clearTimeout(this.timer);
      this.timer = null;
    }
    const values = this.pending;
    this.pending = {};
    const keys = Object.keys(values);
    if (keys.length === 0) return;
    await this.send({ values }, keys);
  }

  private async send(update: { values?: Record<string, ParamValue>; reset?: string[] }, keys: string[]): Promise<void> {
    this.saving = true;
    try {
      const cfg = await api.updateConfig(update);
      const rejected = { ...this.rejected };
      for (const k of keys) delete rejected[k];
      Object.assign(rejected, cfg.rejected ?? {});
      this.rejected = rejected;
      this.requestError = '';
      server.setConfig(cfg);
    } catch (e) {
      this.requestError = e instanceof Error ? e.message : String(e);
    } finally {
      this.saving = false;
    }
  }
}

export const settings = new SettingsStore();
