import { describe, expect, it } from 'vitest';
import { isCompanion, isoDate, roleLabel, splitByRole } from './roles';

describe('roles', () => {
  it('sets aside only what the server says is not a model', () => {
    expect(isCompanion('model')).toBe(false);
    expect(isCompanion('')).toBe(false); // an unreadable header is not known to be a companion
    expect(isCompanion(undefined)).toBe(false); // an older server sends no role
    expect(isCompanion('projector')).toBe(true);
    expect(isCompanion('draft')).toBe(true);
    expect(isCompanion('other')).toBe(true);
  });

  it('names each role', () => {
    expect(roleLabel('projector')).toBe('projector');
    expect(roleLabel('draft')).toBe('draft head');
    expect(roleLabel('other')).toBe('not a model');
  });

  it('puts the models first and keeps the order within each group', () => {
    const { models, companions } = splitByRole([
      { name: 'mmproj-a', role: 'projector' },
      { name: 'a', role: 'model' },
      { name: 'mtp-a', role: 'draft' },
      { name: 'b' },
    ]);
    expect(models.map((m) => m.name)).toEqual(['a', 'b']);
    expect(companions.map((m) => m.name)).toEqual(['mmproj-a', 'mtp-a']);
  });

  it('reads the date of a timestamp', () => {
    expect(isoDate('2026-07-17T13:00:49.000Z')).toBe('2026-07-17');
    expect(isoDate('')).toBe('');
    expect(isoDate(undefined)).toBe('');
  });
});
