import { describe, expect, it } from 'vitest';
import { isAgentCapabilities } from './agent';

describe('isAgentCapabilities', () => {
  it('takes what GET /api/agent answers', () => {
    expect(isAgentCapabilities({ tools: false, commands: false, agents: [] })).toBe(true);
    expect(
      isAgentCapabilities({
        tools: true,
        commands: true,
        agents: [
          { id: 'builtin', name: 'Built-in', available: true },
          { id: 'opencode', name: 'OpenCode', available: false, missing: ['opencode'] },
        ],
      }),
    ).toBe(true);
  });

  it('refuses a shape the composer cannot use', () => {
    expect(isAgentCapabilities({ tools: true, commands: true })).toBe(false);
    expect(isAgentCapabilities({ tools: true, commands: true, agents: [{ id: 'x' }] })).toBe(false);
    expect(isAgentCapabilities(null)).toBe(false);
  });
});
