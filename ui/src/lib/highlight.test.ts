import { describe, expect, it } from 'vitest';
import { escapeHtml, highlight, langOf } from './highlight';

describe('highlight', () => {
  it('marks keywords, strings, numbers and comments', () => {
    const h = highlight('const x = "a" + 42; // note', 'ts');
    expect(h).toContain('<span class="hl-k">const</span>');
    expect(h).toContain('<span class="hl-s">&quot;a&quot;</span>');
    expect(h).toContain('<span class="hl-n">42</span>');
    expect(h).toContain('<span class="hl-c">// note</span>');
  });

  it('uses the comment mark of the language', () => {
    expect(highlight('x = 1  # one', 'python')).toContain('<span class="hl-c"># one</span>');
    expect(highlight('#include <x>', 'c')).not.toContain('hl-c');
  });

  it('escapes everything, so code can never become markup', () => {
    const h = highlight('<script>alert(1)</script>', 'html');
    expect(h).not.toContain('<script>');
    expect(h).toContain('&lt;script&gt;');
  });

  it('does not take digits inside a name for a number', () => {
    expect(highlight('x1 = v2', '')).not.toContain('hl-n');
  });

  it('closes an unterminated string at the end of its line', () => {
    expect(highlight('"open\nnext', 'js')).toBe('<span class="hl-s">&quot;open</span>\nnext');
  });
});

describe('escapeHtml and langOf', () => {
  it('escape the five characters', () => {
    expect(escapeHtml(`<a href="x">'&'</a>`)).toBe('&lt;a href=&quot;x&quot;&gt;&#39;&amp;&#39;&lt;/a&gt;');
  });
  it('read the extension', () => {
    expect(langOf('src/main.cpp')).toBe('cpp');
    expect(langOf('Makefile')).toBe('');
  });
});
