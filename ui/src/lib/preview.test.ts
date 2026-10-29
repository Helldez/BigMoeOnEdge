import { describe, expect, it } from 'vitest';
import { extOf, previewKind } from './preview';

describe('previewKind', () => {
  it('follows the content type', () => {
    expect(previewKind('a', 'text/html; charset=utf-8')).toBe('html');
    expect(previewKind('a', 'image/png')).toBe('image');
    expect(previewKind('a', 'image/svg+xml')).toBe('svg');
    expect(previewKind('a', 'application/octet-stream')).toBe('binary');
  });
  it('falls back on the extension', () => {
    expect(previewKind('docs/README.md', 'text/plain')).toBe('markdown');
    expect(previewKind('src/main.cpp', 'text/plain')).toBe('code');
    expect(previewKind('index.html')).toBe('html');
    expect(previewKind('notes.txt')).toBe('text');
    expect(previewKind('photo.JPG')).toBe('image');
  });
  it('reads extensions, not dotfiles', () => {
    expect(extOf('.gitignore')).toBe('');
    expect(extOf('a/b.tar.gz')).toBe('gz');
  });
});
