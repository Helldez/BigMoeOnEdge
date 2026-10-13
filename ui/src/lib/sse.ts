// Incremental Server-Sent Events parser (WHATWG event-stream rules), used for the chat
// completion stream where EventSource cannot be used (it cannot POST). Network chunks can
// split a line anywhere, so the parser buffers until a line terminator arrives.

export interface SseEvent {
  /** `event:` field, or 'message' when absent. */
  event: string;
  /** All `data:` lines of the event joined with '\n'. */
  data: string;
  id?: string;
}

export class SseParser {
  private buffer = '';
  private dataLines: string[] = [];
  private eventName = '';
  private lastId: string | undefined;
  private hasData = false;
  private pendingCR = false;

  /** Feeds one decoded text chunk; returns the events it completed, in order. */
  push(chunk: string): SseEvent[] {
    const out: SseEvent[] = [];
    let text = chunk;
    // A '\r' at the end of the previous chunk may be the first half of a '\r\n'.
    if (this.pendingCR) {
      this.pendingCR = false;
      if (text.startsWith('\n')) text = text.slice(1);
    }
    this.buffer += text;
    let start = 0;
    for (let i = 0; i < this.buffer.length; i++) {
      const c = this.buffer[i];
      if (c !== '\n' && c !== '\r') continue;
      this.line(this.buffer.slice(start, i), out);
      if (c === '\r') {
        if (i + 1 < this.buffer.length) {
          if (this.buffer[i + 1] === '\n') i++;
        } else {
          this.pendingCR = true;
        }
      }
      start = i + 1;
    }
    this.buffer = this.buffer.slice(start);
    return out;
  }

  /** Ends the stream: a trailing event without its blank line is still delivered. */
  end(): SseEvent[] {
    const out: SseEvent[] = [];
    if (this.buffer.length > 0) this.line(this.buffer, out);
    this.buffer = '';
    this.line('', out);
    return out;
  }

  private line(line: string, out: SseEvent[]): void {
    if (line === '') {
      if (this.hasData) {
        const ev: SseEvent = { event: this.eventName || 'message', data: this.dataLines.join('\n') };
        if (this.lastId !== undefined) ev.id = this.lastId;
        out.push(ev);
      }
      this.dataLines = [];
      this.eventName = '';
      this.hasData = false;
      return;
    }
    if (line.startsWith(':')) return; // comment (keep-alive ping)
    const colon = line.indexOf(':');
    const field = colon < 0 ? line : line.slice(0, colon);
    let value = colon < 0 ? '' : line.slice(colon + 1);
    if (value.startsWith(' ')) value = value.slice(1);
    switch (field) {
      case 'data':
        this.dataLines.push(value);
        this.hasData = true;
        break;
      case 'event':
        this.eventName = value;
        break;
      case 'id':
        this.lastId = value;
        break;
      default:
        break; // 'retry' and unknown fields are irrelevant here
    }
  }
}

/** The OpenAI end-of-stream sentinel. */
export function isDoneSentinel(data: string): boolean {
  return data.trim() === '[DONE]';
}
