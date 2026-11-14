// Every word a template shows comes from the dictionary (en.ts) or from the engine: a component
// names a key, never the text. This reads each .svelte file's markup and fails on a word written
// into it: a text node, a word-bearing attribute (title, aria-label, placeholder...), or a string
// literal shown from an expression in either place. Only what is no word passes: punctuation,
// numbers and unit symbols.
import { parse } from 'svelte/compiler';
import { describe, expect, it } from 'vitest';

const sources = import.meta.glob('../../**/*.svelte', { query: '?raw', import: 'default', eager: true }) as Record<string, string>;

/** Attributes a person reads (as a tooltip, a screen reader's name, a hint in a field), on an
 *  element or passed to a component that shows it. */
const WORD_ATTRS = new Set(['title', 'aria-label', 'aria-description', 'aria-valuetext', 'placeholder', 'alt', 'label', 'hint', 'text', 'description', 'message', 'confirm']);

/** No word: what is left has no letter once these unit symbols are taken out. */
const UNITS = /\b(tok\/s|tok|GiB|MiB|KiB|GB|MB|ms|x|s)\b/g;
export function isWord(s: string): boolean {
  return /\p{L}/u.test(s.replace(UNITS, ''));
}

type Node = { type?: string; [k: string]: unknown };

/** String literals an expression shows: not a key handed to t()/tp(), a value compared against,
 *  an object key, or an argument of any other call (a format, a code), only what it evaluates to. */
function shownLiterals(e: Node | null | undefined, out: string[]): void {
  if (!e || typeof e !== 'object') return;
  switch (e.type) {
    case 'Literal':
      if (typeof e.value === 'string') out.push(e.value);
      return;
    case 'TemplateLiteral':
      for (const q of e.quasis as Node[]) out.push(String((q.value as { cooked: string }).cooked));
      for (const x of e.expressions as Node[]) shownLiterals(x, out);
      return;
    case 'ConditionalExpression':
      shownLiterals(e.consequent as Node, out);
      shownLiterals(e.alternate as Node, out);
      return;
    case 'LogicalExpression':
      shownLiterals(e.left as Node, out);
      shownLiterals(e.right as Node, out);
      return;
    case 'BinaryExpression':
      if (e.operator === '+') {
        shownLiterals(e.left as Node, out);
        shownLiterals(e.right as Node, out);
      }
      return;
    default:
      return; // calls, members, identifiers: what they return is not written here
  }
}

function attrWords(a: Node, where: string, out: string[]): void {
  if (a.type !== 'Attribute' || !WORD_ATTRS.has(String(a.name))) return;
  const parts = a.value === true ? [] : Array.isArray(a.value) ? (a.value as Node[]) : [a.value as Node];
  for (const p of parts) {
    if (p.type === 'Text' && isWord(String(p.data))) out.push(`${where}: ${a.name}="${String(p.data).trim()}"`);
    if (p.type === 'ExpressionTag') {
      const lits: string[] = [];
      shownLiterals(p.expression as Node, lits);
      for (const l of lits) if (isWord(l)) out.push(`${where}: ${a.name}={'${l}'}`);
    }
  }
}

/** Walks the markup only (never the script or the style), collecting every word written in it. */
function walk(n: unknown, where: string, out: string[]): void {
  if (!n || typeof n !== 'object') return;
  if (Array.isArray(n)) {
    for (const c of n) walk(c, where, out);
    return;
  }
  const node = n as Node;
  if (node.type === 'Text') {
    if (isWord(String(node.data))) out.push(`${where}: "${String(node.data).trim()}"`);
    return;
  }
  if (node.type === 'ExpressionTag') {
    const lits: string[] = [];
    shownLiterals(node.expression as Node, lits);
    for (const l of lits) if (isWord(l)) out.push(`${where}: {'${l}'}`);
    return;
  }
  if (node.type === 'StyleSheet' || node.type === 'Script') return;
  if (Array.isArray(node.attributes)) for (const a of node.attributes as Node[]) attrWords(a, where, out);
  for (const [k, v] of Object.entries(node)) {
    if (k === 'attributes' || k === 'expression' || k === 'test' || k === 'key' || k === 'context' || k === 'index') continue;
    if (v && typeof v === 'object') walk(v, where, out);
  }
}

export function templateWords(file: string, source: string): string[] {
  const ast = parse(source, { modern: true });
  const out: string[] = [];
  walk(ast.fragment, file.replace(/^(\.\.\/)+/, ''), out);
  return out;
}

describe('templates', () => {
  it('write no word: every one is a key in the dictionary', () => {
    const found = Object.entries(sources).flatMap(([file, src]) => templateWords(file, src));
    expect(found).toEqual([]);
  });

  it('catch a word in text, in an attribute and in a shown literal', () => {
    expect(templateWords('x.svelte', '<p>Hello</p>')).toHaveLength(1);
    expect(templateWords('x.svelte', '<button title="Close it">x</button>')).toHaveLength(1);
    expect(templateWords('x.svelte', "<p>{busy ? 'Measuring...' : t('a.b')}</p>")).toHaveLength(1);
    expect(templateWords('x.svelte', "<input placeholder={'Type here'} />")).toHaveLength(1);
  });

  it('let through keys, engine words, punctuation, numbers and units', () => {
    const ok = "<p>{t('a.b')} · {plan.sentence} <span>tok/s</span> → 12% {n}x {v === 'ok' ? '+' : '-'}</p><i class=\"a b\"></i>";
    expect(templateWords('x.svelte', ok)).toEqual([]);
  });
});
