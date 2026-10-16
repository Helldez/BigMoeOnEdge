// Choose from options: the engine's decide request (docs/decide.md) as the chat offers it. The
// options are lettered, the question is the request's prefix and the lettered options its
// suffix, and the answer is each option with the probability the model put on it. Pure, so the
// wording of the prompt and the reading of the answer are tested without a server; the Android
// app builds the same request, so a decision reads alike on both.
import type { ChoiceScore, DecideResult } from './types';

/** Single letters, one token each in the usual tokenizers: two options never share a first token. */
export const MAX_OPTIONS = 26;

export function labels(n: number): string[] {
  return Array.from({ length: Math.min(n, MAX_OPTIONS) }, (_, i) => String.fromCharCode(65 + i));
}

/** The options, one per non-blank line. */
export function parseOptions(text: string): string[] {
  return text
    .split('\n')
    .map((l) => l.trim())
    .filter((l) => l !== '')
    .slice(0, MAX_OPTIONS);
}

/**
 * The part of the prompt that changes from one question to the next: the lettered options and
 * the instruction to answer with a letter. The question is the prefix, so asking it again with
 * other options restores it instead of reading it again.
 */
export function suffix(options: string[]): string {
  const ls = labels(options.length);
  return '\n\nOptions:\n' + options.map((o, i) => `${ls[i]}) ${o}\n`).join('') + 'Answer with the letter of one option only.';
}

/** How the question reads in the transcript. */
export function userText(question: string, options: string[]): string {
  const ls = labels(options.length);
  return question + options.map((o, i) => `\n${ls[i]}) ${o}`).join('');
}

export function decideRequest(question: string, options: string[]) {
  return { prefix: question, suffix: suffix(options), choices: labels(options.length) };
}

/** The answer against the options it was asked about: exp(log p), not renormalised. */
export function scores(r: DecideResult, options: string[]): ChoiceScore[] {
  const ls = labels(options.length);
  return options.map((text, i) => {
    const lp = r.choice_logp[i];
    return { label: ls[i], text, prob: typeof lp === 'number' && Number.isFinite(lp) ? Math.exp(lp) : 0 };
  });
}

/** The figure under a decision: what the prompt cost. */
export function decideMetrics(r: DecideResult): string {
  let s = `prefill ${r.prefill_s.toFixed(1)}s (${r.n_prefilled} tok`;
  if (r.n_reused > 0) s += `, ${r.n_reused} reused`;
  s += ')';
  if (r.prefix_state_mib > 0) s += ` · context kept ${Math.round(r.prefix_state_mib)} MiB`;
  return s;
}
