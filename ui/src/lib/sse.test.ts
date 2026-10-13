import { describe, expect, it } from 'vitest';
import { SseParser, isDoneSentinel } from './sse';

describe('SseParser', () => {
  it('parses a single event', () => {
    const p = new SseParser();
    expect(p.push('data: {"a":1}\n\n')).toEqual([{ event: 'message', data: '{"a":1}' }]);
  });

  it('joins lines split across chunks, even mid-field', () => {
    const p = new SseParser();
    expect(p.push('da')).toEqual([]);
    expect(p.push('ta: hel')).toEqual([]);
    expect(p.push('lo\n')).toEqual([]);
    expect(p.push('\n')).toEqual([{ event: 'message', data: 'hello' }]);
  });

  it('returns several events from one chunk, in order', () => {
    const p = new SseParser();
    const out = p.push('data: 1\n\ndata: 2\n\nevent: token\ndata: 3\n\n');
    expect(out.map((e) => e.data)).toEqual(['1', '2', '3']);
    expect(out[2].event).toBe('token');
  });

  it('ignores comments and does not emit events for them', () => {
    const p = new SseParser();
    expect(p.push(': ping\n\n')).toEqual([]);
    expect(p.push(': ping\ndata: x\n\n')).toEqual([{ event: 'message', data: 'x' }]);
  });

  it('joins multi-line data with newlines', () => {
    const p = new SseParser();
    expect(p.push('data: a\ndata: b\n\n')[0].data).toBe('a\nb');
  });

  it('handles CRLF, including a CR/LF pair split across chunks', () => {
    const p = new SseParser();
    expect(p.push('data: a\r')).toEqual([]);
    expect(p.push('\n\r\n')).toEqual([{ event: 'message', data: 'a' }]);
    expect(p.push('data: b\r\rdata: c\r\n\r\n').map((e) => e.data)).toEqual(['b', 'c']);
  });

  it('strips only one leading space from the value', () => {
    const p = new SseParser();
    expect(p.push('data:  two\n\ndata:none\n\n').map((e) => e.data)).toEqual([' two', 'none']);
  });

  it('delivers [DONE] as a data event and recognises it', () => {
    const p = new SseParser();
    const out = p.push('data: {"x":1}\n\ndata: [DONE]\n\n');
    expect(out).toHaveLength(2);
    expect(isDoneSentinel(out[0].data)).toBe(false);
    expect(isDoneSentinel(out[1].data)).toBe(true);
  });

  it('flushes a trailing event without its blank line on end()', () => {
    const p = new SseParser();
    expect(p.push('data: tail')).toEqual([]);
    expect(p.end()).toEqual([{ event: 'message', data: 'tail' }]);
  });

  it('resets the event name between events', () => {
    const p = new SseParser();
    const out = p.push('event: state\ndata: {}\n\ndata: {}\n\n');
    expect(out.map((e) => e.event)).toEqual(['state', 'message']);
  });
});
