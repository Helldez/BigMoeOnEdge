// App-wide server state, fed by one EventSource on `GET /api/events` for the whole app.
import { api, EVENTS_URL } from '../lib/api';
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

  ready = $derived(this.info?.state === 'ready' && this.info.model !== null);

  private source: EventSource | null = null;
  private retryMs = RECONNECT_MIN_MS;
  private retryTimer: ReturnType<typeof setTimeout> | null = null;

  start(): void {
    if (this.source) return;
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
    const es = new EventSource(EVENTS_URL);
    this.source = es;
    es.onopen = () => {
      this.connected = true;
      this.retryMs = RECONNECT_MIN_MS;
    };
    // The browser retries some failures by itself but gives up on others (readyState CLOSED);
    // closing and reconnecting with backoff covers both the same way.
    es.onerror = () => {
      this.connected = false;
      es.close();
      this.source = null;
      this.scheduleReconnect();
    };
    es.addEventListener('state', (e) => this.onJson<Info>(e, (d) => (this.info = d)));
    es.addEventListener('config', (e) => this.onJson<Config>(e, (d) => (this.config = d)));
    es.addEventListener('token', (e) => this.onJson<TokenEvent>(e, (d) => this.onToken(d)));
    es.addEventListener('done', (e) => this.onJson<DoneSummary>(e, (d) => (this.lastDone = d)));
    es.addEventListener('download', (e) => this.onJson<DownloadEvent>(e, (d) => this.onDownload(d)));
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

  private onJson<T>(e: Event, apply: (data: T) => void): void {
    try {
      apply(JSON.parse((e as MessageEvent<string>).data) as T);
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
