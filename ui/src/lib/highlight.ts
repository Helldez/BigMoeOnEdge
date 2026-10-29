// A small, language-agnostic code highlighter: comments, strings, numbers and the keywords most
// languages share, marked with classes (`hl-*` in app.css). It works on text and returns escaped
// HTML, so what it highlights can never carry markup of its own. Good enough to read code at a
// glance; it is not a parser, and an unknown language reads as plain text with these marks.

const KEYWORDS = new Set(
  (
    'as async await break case catch class const continue def default defer del do elif else enum export extends ' +
    'false final finally fn for from func function go if impl import in interface let loop match mod module mut ' +
    'namespace new nil none null package pass private protected pub public raise return self static struct super ' +
    'switch this throw true try type typedef typeof union unsafe use using val var void where while with yield ' +
    'True False None auto bool char double float int long short signed unsigned include define template'
  ).split(' '),
);

export function escapeHtml(s: string): string {
  return s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]!);
}

// Which comment marks a language uses: `#` for the shell family, `//` and `/* */` for the C family,
// `--` for SQL and Lua. Unknown: both of the common ones.
function commentStyle(lang: string): { line: string[]; block: boolean } {
  const l = lang.toLowerCase();
  if (/^(py|python|sh|bash|zsh|shell|console|yaml|yml|toml|rb|ruby|r|perl|pl|makefile|make|dockerfile|ini|conf|cmake|ps1|powershell)$/.test(l))
    return { line: ['#'], block: false };
  if (/^(sql|lua|hs|haskell)$/.test(l)) return { line: ['--'], block: false };
  if (/^(json|md|markdown|text|txt|plain|csv|diff)$/.test(l)) return { line: [], block: false };
  if (l === '') return { line: ['//', '#'], block: true };
  return { line: ['//'], block: true };
}

const NUMBER = /^(0x[0-9a-fA-F_]+|\d[\d_]*(\.\d+)?([eE][+-]?\d+)?)/;
const WORD = /^[A-Za-z_$][\w$]*/;

/** `code` as HTML with highlight spans; `lang` is a fence's language name, '' when unknown. */
export function highlight(code: string, lang = ''): string {
  const style = commentStyle(lang);
  const out: string[] = [];
  let i = 0;
  const span = (cls: string, text: string) => out.push(`<span class="${cls}">${escapeHtml(text)}</span>`);
  while (i < code.length) {
    const rest = code.slice(i);
    const lineMark = style.line.find((m) => rest.startsWith(m));
    if (lineMark) {
      const end = code.indexOf('\n', i);
      const stop = end < 0 ? code.length : end;
      span('hl-c', code.slice(i, stop));
      i = stop;
      continue;
    }
    if (style.block && rest.startsWith('/*')) {
      const end = code.indexOf('*/', i + 2);
      const stop = end < 0 ? code.length : end + 2;
      span('hl-c', code.slice(i, stop));
      i = stop;
      continue;
    }
    const c = code[i];
    if (c === '"' || c === "'" || c === '`') {
      let j = i + 1;
      while (j < code.length && code[j] !== c && !(code[j] === '\n' && c !== '`')) j += code[j] === '\\' ? 2 : 1;
      const stop = j < code.length && code[j] === c ? j + 1 : Math.min(j, code.length);
      span('hl-s', code.slice(i, stop));
      i = stop;
      continue;
    }
    const prev = i > 0 ? code[i - 1] : '';
    if (/\d/.test(c) && !/[\w$]/.test(prev)) {
      const m = NUMBER.exec(rest);
      if (m) {
        span('hl-n', m[0]);
        i += m[0].length;
        continue;
      }
    }
    const w = WORD.exec(rest);
    if (w) {
      if (KEYWORDS.has(w[0])) span('hl-k', w[0]);
      else out.push(escapeHtml(w[0]));
      i += w[0].length;
      continue;
    }
    out.push(escapeHtml(c));
    i++;
  }
  return out.join('');
}

/** A file's language by its extension, for a fence-less preview. */
export function langOf(path: string): string {
  const ext = path.toLowerCase().split('.').pop() ?? '';
  return ext === path.toLowerCase() ? '' : ext;
}
