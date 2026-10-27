// App-wide server state, fed by one `GET /api/events` stream held open for the whole app.
import { api, openEvents } from '../lib/api';
import { auth } from '../lib/auth';
import type { SseEvent } from '../lib/sse';
import { isConfig, isDoneSummary, isDownloadEvent, isInfo, isPlanProgress, isTokenEvent } from '../lib/guards';
import { ema, mibPerSec, pushSample, tokPerSec, wallShare } from '../lib/metrics';
import type { Config, DoneSummary, DownloadEvent, Info, ParamSpec, Plan, PlanProgress, TokenEvent } from '../lib/types';

const HISTORY = 120;
const RECONNECT_MIN_MS = 1000;
const RECONNECT_MAX_MS = 15000;

export interface LiveMetrics {
  last: TokenEvent | null;
  tokS: number | null;
  readMibS: number | null;
  /** Share of the wall the flash wait took (the term the server chose for the run's mode). */
  flashWait: number | null;
  tokHistory: number[];
  readHistory: number[];
}

function emptyMetrics(): LiveMetrics {
  return { last: null, tokS: null, readMibS: null, flashWait: null, tokHistory: [], readHistory: [] };
}

class ServerStore {
  info = $state<Info | null>(null);
  config = $state<Config | null>(null);
  params = $state<ParamSpec[] | null>(null);
  paramsError = $state('');
  connected = $state(false);
  metrics = $state<LiveMetrics>(emptyMetrics());
  lastDone = $state<DoneSummary | null>(null);
  /** The stage a measurement is in, null when none runs (a `done` stage clears it). */
  planProgress = $state<PlanProgress | null>(null);
  /**
   * The plan the server holds for the chosen model (`GET /api/plan?cached=1`, never a measurement):
   * read when the server is reached, when a session opens and when a turn ends, so the Plan page
   * opens on it at once and its measured figures land when the answer does, whichever page is open.
   */
  plan = $state<Plan | null>(null);
  planError = $state('');
  /** A measurement asked for (`?measure=1`) is in flight: the person's tap, never automatic. */
  measuring = $state(false);
  downloads = $state<Record<string, DownloadEvent>>({});
  /** Bumped when a download ends, so the Models page knows to refresh its list. */
  downloadsFinished = $state(0);
  /** The server wants an API key this page has not got: the app asks for one. */
  authRequired = $state(false);

  ready = $derived(this.info?.state === 'ready' && this.info.model !== null);

  private stream: AbortController | null = null;
  private retryMs = RECONNECT_MIN_MS;
  private openedBefore = false;
  private retryTimer: ReturnType<typeof setTimeout> | null = null;

  start(): void {
    if (this.stream) return;
    auth.onUnauthorized(() => {
      this.authRequired = true;
    });
    this.connect();
    this.refreshStatic();
  }

  /** The key the person typed: kept for this browser, and everything is asked again with it. */
  submitKey(key: string): void {
    auth.set(key);
    this.authRequired = false;
    this.retryMs = RECONNECT_MIN_MS;
    this.connect();
    this.refreshStatic();
  }

  /**
   * What the event stream does not carry from its start: the schema, and the config, which only
   * arrives as an event when it changes. Asked again on every (re)connection, since a server
   * restarted meanwhile may have a new schema and a config this page never saw change.
   */
  refreshStatic(): void {
    this.loadParams();
    this.loadConfig();
    this.refreshPlan();
  }

  /** The plan held, without measuring; skipped while a measurement runs, which answers instead. */
  async refreshPlan(): Promise<void> {
    if (this.measuring) return;
    try {
      this.plan = await api.plan('cached');
      this.planError = '';
    } catch (e) {
      this.planError = e instanceof Error ? e.message : String(e);
    }
  }

  /** Measure the machine for the chosen model: the probes run, and the plan shown is the new one. */
  async measurePlan(): Promise<void> {
    if (this.measuring) return;
    this.measuring = true;
    this.planError = '';
    try {
      this.plan = await api.plan('measure');
    } catch (e) {
      this.planError = e instanceof Error ? e.message : String(e);
    } finally {
      this.measuring = false;
    }
  }

  async loadConfig(): Promise<void> {
    try {
      this.config = await api.config();
    } catch {
      // unreachable or refused: the next connection asks again, and a 401 already asked for a key
    }
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
        // A reconnection may follow a server restart: the schema and the config can have changed,
        // and a config change made while this page was cut off arrived as no event.
        if (this.openedBefore) this.refreshStatic();
        this.openedBefore = true;
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
        this.onJson<Info>(ev, isInfo, (d) => this.onInfo(d));
        break;
      case 'config':
        this.onJson<Config>(ev, isConfig, (d) => (this.config = d));
        break;
      case 'token':
        this.onJson<TokenEvent>(ev, isTokenEvent, (d) => this.onToken(d));
        break;
      case 'done':
        this.onJson<DoneSummary>(ev, isDoneSummary, (d) => {
          this.lastDone = d;
          // A finished turn is a new measurement beside the plan the model runs with.
          if (this.info?.model) this.refreshPlan();
        });
        break;
      case 'download':
        this.onJson<DownloadEvent>(ev, isDownloadEvent, (d) => this.onDownload(d));
        break;
      case 'plan_progress':
        this.onJson<PlanProgress>(ev, isPlanProgress, (d) => (this.planProgress = d.step === 'done' ? null : d));
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
    }, this.retryMs);
    this.retryMs = Math.min(this.retryMs * 2, RECONNECT_MAX_MS);
  }

  private onJson<T>(e: SseEvent, guard: (v: unknown) => v is T, apply: (data: T) => void): void {
    let data: unknown;
    try {
      data = JSON.parse(e.data);
    } catch {
      return; // a malformed event is dropped; the next one carries full state again
    }
    if (guard(data)) apply(data); // and so is one that is not the shape the UI reads
  }

  private onInfo(d: Info): void {
    const before = this.info;
    this.info = d;
    // A new session runs a plan of its own (an auto load's, or the settings as they were).
    const opened = d.state === 'ready' && d.model !== null && (before?.model?.path !== d.model.path || before?.model?.load_s !== d.model.load_s);
    if (opened) this.refreshPlan();
  }

  private onToken(t: TokenEvent): void {
    const m = this.metrics;
    const tok = tokPerSec(t.wall_ms);
    const read = mibPerSec(t.read_mib, t.wall_ms);
    this.metrics = {
      last: t,
      tokS: ema(m.tokS, tok),
      readMibS: ema(m.readMibS, read),
      flashWait: ema(m.flashWait, wallShare(t.flash_wait_ms ?? 0, t.wall_ms)),
      tokHistory: pushSample(m.tokHistory, tok, HISTORY),
      readHistory: pushSample(m.readHistory, read, HISTORY),
    };
  }

  /** Drop a download's last event: its partial files were deleted, so nothing is left to resume. */
  forgetDownload(id: string): void {
    const { [id]: _gone, ...rest } = this.downloads;
    this.downloads = rest;
  }

  private onDownload(d: DownloadEvent): void {
    this.downloads = { ...this.downloads, [d.id]: d };
    if (d.state !== 'running') this.downloadsFinished++;
  }
}

export const server = new ServerStore();
