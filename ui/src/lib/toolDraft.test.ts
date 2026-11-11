import { describe, expect, it } from 'vitest';
import { draftText, tailLines } from './toolDraft';

describe('draftText', () => {
  it('reads the content a file call is writing, decoded', () => {
    const raw = '{"path":"page.html","content":"<!DOCTYPE html>\\n<html>\\n  <p class=\\"x\\">hi';
    expect(draftText(raw)).toBe('<!DOCTYPE html>\n<html>\n  <p class="x">hi');
  });

  it('is the value being written, even when a finished one is longer', () => {
    expect(draftText('{"path":"some/long/path.html","content":"<p')).toBe('<p');
  });

  it('with none open, is the longest value, never a key', () => {
    expect(draftText('{"a_very_long_key_name":"x","content":"ab"')).toBe('ab');
    expect(draftText('{"path":"some/long/path.html","content":"<p>"}')).toBe('some/long/path.html');
  });

  it('leaves out an escape cut at the end until the rest arrives', () => {
    expect(draftText('{"content":"a\\')).toBe('a');
    expect(draftText('{"content":"a\\u00')).toBe('a');
    expect(draftText('{"content":"a\\u00e9')).toBe('aé');
  });

  it('is empty before a value starts', () => {
    expect(draftText('')).toBe('');
    expect(draftText('{"pa')).toBe('');
    expect(draftText('{"path":')).toBe('');
  });

  it('reads values inside arrays', () => {
    expect(draftText('{"edits":["one","a longer one')).toBe('a longer one');
  });

  it('reads a closed call whole', () => {
    expect(draftText('{"command":"npm test","cwd":"."}')).toBe('npm test');
  });
});

describe('tailLines', () => {
  it('keeps the newest lines', () => {
    expect(tailLines('1\n2\n3\n4', 2)).toBe('3\n4');
    expect(tailLines('1\n2', 5)).toBe('1\n2');
  });
});
