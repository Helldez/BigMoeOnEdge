import { describe, expect, it } from 'vitest';
import { commandLine, formatBytes, progress, quoteArg } from './format';
import { parseHash } from './route';
import { planDecisions } from './plan';

describe('format', () => {
  it('formats bytes in binary units', () => {
    expect(formatBytes(512)).toBe('512 B');
    expect(formatBytes(1536)).toBe('1.5 KiB');
    expect(formatBytes(18556686912)).toBe('17.3 GiB');
  });

  it('quotes only arguments that need it', () => {
    expect(quoteArg('--threads')).toBe('--threads');
    expect(quoteArg('C:/my models/a.gguf')).toBe('"C:/my models/a.gguf"');
    expect(quoteArg('')).toBe('""');
  });

  it('builds the bmoe-cli command line', () => {
    expect(commandLine(['--model', 'a b.gguf', '--threads', '8'])).toBe('bmoe-cli --model "a b.gguf" --threads 8');
  });

  it('progress is null without a total', () => {
    expect(progress(5, 0)).toBeNull();
    expect(progress(5, 10)).toBe(0.5);
  });
});

describe('route', () => {
  it('parses known routes and falls back to chat', () => {
    expect(parseHash('#/models')).toBe('models');
    expect(parseHash('#/settings?x=1')).toBe('settings');
    expect(parseHash('')).toBe('chat');
    expect(parseHash('#/nope')).toBe('chat');
  });
});

describe('planDecisions', () => {
  const d = { knob: 'k', value: '1', source: 'measured', reason: 'r' };
  it('accepts an array, an object with decisions, or null', () => {
    expect(planDecisions([d])).toEqual([d]);
    expect(planDecisions({ decisions: [d] })).toEqual([d]);
    expect(planDecisions(null)).toEqual([]);
  });
});
