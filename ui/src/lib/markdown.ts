// Markdown for model output. Everything the model writes is untrusted, so the HTML marked
// produces always goes through DOMPurify before it reaches the DOM.
import { marked } from 'marked';
import DOMPurify from 'dompurify';

marked.setOptions({ gfm: true, breaks: true });

// An image is fetched the moment an answer is shown, with no click: text that talks the model into
// writing one can send what its URL carries to any host, or have the browser GET one of this
// server's own endpoints. Only an image that carries its bytes inline (a data: URL) fetches nothing,
// so that is the only kind kept; any other becomes its alt text.
const INLINE_IMAGE = /^data:image\/(png|gif|jpeg|webp);/i;

DOMPurify.addHook('afterSanitizeAttributes', (node) => {
  // Links open outside the app and never get the opener.
  if (node.tagName === 'A') {
    node.setAttribute('target', '_blank');
    node.setAttribute('rel', 'noopener noreferrer');
  }
  if (node.tagName === 'IMG' && !INLINE_IMAGE.test(node.getAttribute('src') ?? '')) {
    node.replaceWith(node.ownerDocument.createTextNode(node.getAttribute('alt') ?? ''));
  }
});

// An answer is text to read, never a control to operate. The HTML profile alone lets a form
// through, and a form the model was talked into writing is a same-origin request one click away
// from the server's API; a style block or attribute can redress the page around it. Media and
// srcset would fetch from anywhere like an image does; the server's Content-Security-Policy stops
// those fetches as well, this keeps them out of the page in the first place.
const SANITIZE_CONFIG = {
  USE_PROFILES: { html: true },
  FORBID_TAGS: [
    'style',
    'form',
    'input',
    'button',
    'textarea',
    'select',
    'option',
    'picture',
    'source',
    'video',
    'audio',
    'track',
  ],
  FORBID_ATTR: ['style', 'action', 'formaction', 'srcset', 'background', 'poster'],
};

export function renderMarkdown(text: string): string {
  const html = marked.parse(text, { async: false }) as string;
  return DOMPurify.sanitize(html, SANITIZE_CONFIG);
}
