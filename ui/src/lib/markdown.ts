// Markdown for model output. Everything the model writes is untrusted, so the HTML marked
// produces always goes through DOMPurify before it reaches the DOM.
import { marked } from 'marked';
import DOMPurify from 'dompurify';

marked.setOptions({ gfm: true, breaks: true });

// Links open outside the app and never get the opener.
DOMPurify.addHook('afterSanitizeAttributes', (node) => {
  if (node.tagName === 'A') {
    node.setAttribute('target', '_blank');
    node.setAttribute('rel', 'noopener noreferrer');
  }
});

// An answer is text to read, never a control to operate. The HTML profile alone lets a form
// through, and a form the model was talked into writing is a same-origin request one click away
// from the server's API; a style block or attribute can redress the page around it.
const SANITIZE_CONFIG = {
  USE_PROFILES: { html: true },
  FORBID_TAGS: ['style', 'form', 'input', 'button', 'textarea', 'select', 'option'],
  FORBID_ATTR: ['style', 'action', 'formaction'],
};

export function renderMarkdown(text: string): string {
  const html = marked.parse(text, { async: false }) as string;
  return DOMPurify.sanitize(html, SANITIZE_CONFIG);
}
