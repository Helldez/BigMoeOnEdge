// App-wide server state, fed by one `GET /api/events` stream held open for the whole app.
import { api, openEvents } from '../lib/api';
import { auth } from '../lib/auth';
import type { SseEvent } from '../lib/sse';
import { ema, mibPerSec, pushSample, stallShare, tokPerSec } from '../lib/metrics';
import type { Config, DoneSummary, DownloadEvent, Info, ParamSpec, TokenEvent } from '../lib/types';

const HISTORY = 120;
const RECONNECT_MIN_MS = 1000;
const RECONNECT_MAX_MS = 15000;

export interface LiveMetrics {
  last: TokenEvent | null;
  tokS: number | null;
  readMibS: number | null;
  stall: number | null;
  tokHistory: number[];
  readHistory: number[];
}

function emptyMetrics(): LiveMetrics {
  return { last: null, tokS: null, readMibS: null, stall: null, tokHistory: [], readHistory: [] };
}

class ServerStore {
  info = $state<Info | null>(null);
  config = $state<Config | null>(null);
  params = $state<ParamSpec[] | null>(null);
  paramsError = $state('');
  connected = $state(false);
  metrics = $state<LiveMetrics>(emptyMetrics());
  lastDone = $state<DoneSummary | null>(null);
  downloads = $state<Record<string, DownloadEvent>>({});
  /** Bumped when a download ends, so the Models page knows to refresh its list. */
  downloadsFinished = $state(0);
  /** The server wants an API key this page has not got: the app asks for one. */
  authRequired = $state(false);

  ready = $derived(this.info?.state === 'ready' && this.info.model !== null);

  private stream: AbortController | null = null;
  private retryMs = RECONNECT_MIN_MS;
  private retryTimer: ReturnType<typeof setTimeout> | null = null;

  start(): void {
    if (this.stream) return;
    auth.onUnauthorized(() => {
      this.authRequired = true;
    });
    this.connect();
    this.loadParams();
    api.config().then((c) => (this.config = c), () => {});
  }

  /** The key the person typed: kept for this browser, and everything is asked again with it. */
  submitKey(key: string): void {
    auth.set(key);
    this.authRequired = false;
    this.retryMs = RECONNECT_MIN_MS;
    this.connect();
    this.loadParams();
    api.config().then((c) => (this.config = c), () => {});
  }

  async loadParams(): Promise<void> {
    try {
      this.params = await api.params();
      this.paramsError = '';
    } catch (e) {
      this.paramsError = e instanceof Error ? e.message : String(e);
    }
  }

  /** Clears the live metrics at the start of a generation. */
  resetMetrics(): void {
    this.metrics = emptyMetrics();
  }

  setConfig(c: Config): void {
    this.config = c;
  }

  private connect(): void {
    if (this.stream) return;
    const ac = new AbortController();
    this.stream = ac;
    openEvents(
      () => {
        this.connected = true;
        this.retryMs = RECONNECT_MIN_MS;
      },
      (ev) => this.onEvent(ev),
      ac.signal,
    )
      .catch(() => {
        // a refused or failed connection: reconnecting below covers it, unless a key is wanted
      })
      .finally(() => {
        if (this.stream !== ac) return; // replaced, or stopped
        this.stream = null;
        this.connected = false;
        if (!this.authRequired) this.scheduleReconnect();
      });
  }

  private onEvent(ev: SseEvent): void {
    switch (ev.event) {
      case 'state':
        this.onJson<Info>(ev, (d) => (this.info = d));
        break;
      case 'config':
        this.onJson<Config>(ev, (d) => (this.config = d));
        break;
      case 'token':
        this.onJson<TokenEvent>(ev, (d) => this.onToken(d));
        break;
      case 'done':
        this.onJson<DoneSummary>(ev, (d) => (this.lastDone = d));
        break;
      case 'download':
        this.onJson<DownloadEvent>(ev, (d) => this.onDownload(d));
        break;
      default:
        break; // a ping, or an event this UI does not know
    }
  }

  private scheduleReconnect(): void {
    if (this.retryTimer) return;
    this.retryTimer = setTimeout(() => {
      this.retryTimer = null;
      this.connect();
      // A reconnect may follow a server restart: the schema can have changed too.
      this.loadParams();
    }, this.retryMs);
    this.retryMs = Math.min(this.retryMs * 2, RECONNECT_MAX_MS);
  }

  private onJson<T>(e: SseEvent, apply: (data: T) => void): void {
    try {
      apply(JSON.parse(e.data) as T);
    } catch {
      // a malformed event is dropped; the next one carries full state again
    }
  }

  private onToken(t: TokenEvent): void {
    const m = this.metrics;
    const tok = tokPerSec(t.wall_ms);
    const read = mibPerSec(t.read_mib, t.wall_ms);
    this.metrics = {
      last: t,
      tokS: ema(m.tokS, tok),
      readMibS: ema(m.readMibS, read),
      stall: ema(m.stall, stallShare(t.stall_ms, t.wall_ms)),
      tokHistory: pushSample(m.tokHistory, tok, HISTORY),
      readHistory: pushSample(m.readHistory, read, HISTORY),
    };
  }

  private onDownload(d: DownloadEvent): void {
    this.downloads = { ...this.downloads, [d.id]: d };
    if (d.state !== 'running') this.downloadsFinished++;
  }
}

export const server = new ServerStore();
