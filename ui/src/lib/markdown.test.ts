// @vitest-environment jsdom
import { describe, expect, it } from 'vitest';
import { renderMarkdown } from './markdown';

// The sanitiser needs a DOM, so this file alone runs in one.
const dom = (markdown: string): HTMLElement => {
  const host = document.createElement('div');
  host.innerHTML = renderMarkdown(markdown);
  return host;
};

describe('renderMarkdown', () => {
  it('drops a form and every control in it, keeping the words', () => {
    const host = dom(
      '<form action="/api/models/download" method="post" enctype="text/plain">' +
        '<input name=\'{"id":"big","x":"\' value=\'"}\'>' +
        '<textarea name="t">body</textarea><select name="s"><option>one</option></select>' +
        '<button formaction="/api/session/load">Continue</button></form>',
    );
    expect(host.querySelector('form, input, button, textarea, select, option')).toBeNull();
    expect(host.innerHTML).not.toMatch(/action|enctype|\/api\//);
    expect(host.textContent).toContain('Continue');
  });

  it('drops controls written outside a form', () => {
    const host = dom('Click <button formaction="/api/cancel" form="f">here</button> <input form="f" value="x">');
    expect(host.querySelector('button, input')).toBeNull();
    expect(host.innerHTML).not.toContain('formaction');
  });

  it('drops style blocks and style attributes', () => {
    const host = dom(
      '<style>body { display: none }</style>\n\n<div style="position:fixed;inset:0">over</div>\n\n<a href="https://e.org" style="position:fixed">l</a>',
    );
    expect(host.querySelector('style')).toBeNull();
    expect(host.querySelector('[style]')).toBeNull();
    expect(host.innerHTML).not.toContain('display: none');
    expect(host.textContent).toContain('over');
  });

  it('still stops scripts and handlers', () => {
    const host = dom('<script>alert(1)</script><img src="x" onerror="alert(1)"><a href="javascript:alert(1)">x</a>');
    expect(host.querySelector('script')).toBeNull();
    expect(host.innerHTML).not.toMatch(/onerror|javascript:/);
  });

  it('opens links outside the app, without the opener', () => {
    const a = dom('[site](https://example.org/a?b=1)').querySelector('a')!;
    expect(a.getAttribute('href')).toBe('https://example.org/a?b=1');
    expect(a.getAttribute('target')).toBe('_blank');
    expect(a.getAttribute('rel')).toBe('noopener noreferrer');
    expect(a.textContent).toBe('site');
  });

  it('renders ordinary markdown as before', () => {
    expect(renderMarkdown('**bold** and *em* and `code`')).toBe(
      '<p><strong>bold</strong> and <em>em</em> and <code>code</code></p>\n',
    );
    expect(renderMarkdown('```js\nconst a = "<b>";\n```')).toBe(
      '<pre><code class="language-js">const a = "&lt;b&gt;";\n</code></pre>\n',
    );
    const list = dom('- one\n- two\n\n1. first\n2. second');
    expect([...list.querySelectorAll('ul > li')].map((li) => li.textContent)).toEqual(['one', 'two']);
    expect([...list.querySelectorAll('ol > li')].map((li) => li.textContent)).toEqual(['first', 'second']);
    const table = dom('| a | b |\n|---|---|\n| 1 | 2 |');
    expect([...table.querySelectorAll('thead th')].map((c) => c.textContent)).toEqual(['a', 'b']);
    expect([...table.querySelectorAll('tbody td')].map((c) => c.textContent)).toEqual(['1', '2']);
    const img = dom('![alt text](https://example.org/p.png "t")').querySelector('img')!;
    expect(img.getAttribute('src')).toBe('https://example.org/p.png');
    expect(img.getAttribute('alt')).toBe('alt text');
    expect(dom('# Title\n\n> quote\n\nline one\nline two').innerHTML).toBe(
      '<h1>Title</h1>\n<blockquote>\n<p>quote</p>\n</blockquote>\n<p>line one<br>line two</p>\n',
    );
  });
});
