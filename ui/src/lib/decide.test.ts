import { describe, expect, it } from 'vitest';
import { decideMetrics, decideRequest, parseOptions, scores, suffix, userText } from './decide';

describe('decide', () => {
  it('letters the options and words the request the way the engine expects', () => {
    const options = parseOptions(' Osaka \n\nTokyo\nKyoto\n');
    expect(options).toEqual(['Osaka', 'Tokyo', 'Kyoto']);
    const req = decideRequest('Capital of Japan?', options);
    expect(req.choices).toEqual(['A', 'B', 'C']);
    expect(req.prefix).toBe('Capital of Japan?');
    expect(req.suffix).toBe(suffix(options));
    expect(req.suffix).toBe('\n\nOptions:\nA) Osaka\nB) Tokyo\nC) Kyoto\nAnswer with the letter of one option only.');
    expect(userText('Capital of Japan?', options)).toBe('Capital of Japan?\nA) Osaka\nB) Tokyo\nC) Kyoto');
  });

  it('reads the answer back as probabilities over the options, null as none', () => {
    const r = {
      cancelled: false,
      best: 1,
      choice_logp: [Math.log(0.1), Math.log(0.85), null],
      n_tokens: 50,
      n_reused: 20,
      n_prefilled: 30,
      prefill_s: 1.26,
      prefix_state_mib: 48.2,
    };
    const s = scores(r, ['Osaka', 'Tokyo', 'Kyoto']);
    expect(s.map((x) => x.label)).toEqual(['A', 'B', 'C']);
    expect(s[1].prob).toBeCloseTo(0.85);
    expect(s[2].prob).toBe(0);
    expect(decideMetrics(r)).toBe('prefill 1.3s (30 tok, 20 reused) · context kept 48 MiB');
  });
});
