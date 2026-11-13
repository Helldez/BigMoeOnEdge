// What a tool call says while the model is still writing it. The arguments arrive as a JSON object
// cut anywhere (`{"path":"a.html","content":"<!DOCTYPE html>\n<ht`), and a person wants to read the
// text being written, not its escapes. Pure: the card calls it on every piece.

/** The lines of a draft a card shows: the newest, as a terminal shows the end of a long output. */
export const DRAFT_TAIL_LINES = 16;

/**
 * The string value a JSON object prefix is writing, decoded as far as it goes: the one still open
 * at the end, which is what the model is writing now (a file's content); with none open, the
 * longest (a finished command line). Keys are not values. An escape cut at the end is left out
 * until the rest of it arrives. '' when no value has started.
 */
export function draftText(raw: string): string {
  let best = '';
  let i = 0;
  let expectKey = false; // inside an object, the next string is a key
  let depth = 0;
  while (i < raw.length) {
    const ch = raw[i];
    if (ch === '{') {
      depth++;
      expectKey = true;
      i++;
    } else if (ch === '}') {
      depth--;
      expectKey = false;
      i++;
    } else if (ch === ',') {
      expectKey = depth > 0 && !inArray(raw, i);
      i++;
    } else if (ch === ':') {
      expectKey = false;
      i++;
    } else if (ch === '"') {
      const { text, end, closed } = readString(raw, i + 1);
      if (!expectKey && !closed) return text;
      if (!expectKey && text.length > best.length) best = text;
      expectKey = false;
      i = end;
    } else {
      i++;
    }
  }
  return best;
}

/** The last `n` lines of a text. */
export function tailLines(text: string, n: number = DRAFT_TAIL_LINES): string {
  const lines = text.split('\n');
  return lines.length <= n ? text : lines.slice(-n).join('\n');
}

// Whether the comma at `at` separates array elements rather than object members: the nearest
// unclosed bracket before it, outside strings, is a '['.
function inArray(raw: string, at: number): boolean {
  const stack: string[] = [];
  let inStr = false;
  for (let k = 0; k < at; k++) {
    const c = raw[k];
    if (inStr) {
      if (c === '\\') k++;
      else if (c === '"') inStr = false;
    } else if (c === '"') inStr = true;
    else if (c === '{' || c === '[') stack.push(c);
    else if (c === '}' || c === ']') stack.pop();
  }
  return stack[stack.length - 1] === '[';
}

const SIMPLE: Record<string, string> = { n: '\n', t: '\t', r: '\r', b: '\b', f: '\f', '"': '"', '\\': '\\', '/': '/' };

// Decodes a JSON string starting after its opening quote, up to the closing quote or the end of
// the prefix. Returns the text, the index after the string, and whether its closing quote came.
function readString(raw: string, from: number): { text: string; end: number; closed: boolean } {
  let out = '';
  let k = from;
  while (k < raw.length) {
    const c = raw[k];
    if (c === '"') return { text: out, end: k + 1, closed: true };
    if (c !== '\\') {
      out += c;
      k++;
      continue;
    }
    const e = raw[k + 1];
    if (e === undefined) break; // the escape is cut: wait for the rest
    if (e in SIMPLE) {
      out += SIMPLE[e];
      k += 2;
    } else if (e === 'u') {
      const hex = raw.slice(k + 2, k + 6);
      if (hex.length < 4) break;
      out += String.fromCharCode(parseInt(hex, 16));
      k += 6;
    } else {
      out += e;
      k += 2;
    }
  }
  return { text: out, end: raw.length, closed: false };
}
