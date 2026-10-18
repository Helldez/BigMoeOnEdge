// Runtime checks on what the server sends, for the parts of each shape the UI reads. A typed
// cast trusts the wire; these make a server of another version, a proxy's error page or a
// truncated body fail where it is read, with a message, instead of as an undefined deep inside a
// component. They check structure, not every field: a field the UI does not read may change freely.
import type { Config, DecideResult, DoneSummary, DownloadEvent, Info, ModelsResponse, ParamSpec, Plan, TokenEvent } from './types';

type Obj = Record<string, unknown>;

export function isObject(v: unknown): v is Obj {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}

const isStr = (v: unknown): v is string => typeof v === 'string';
const isNum = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v);
const isBool = (v: unknown): v is boolean => typeof v === 'boolean';

export function isInfo(v: unknown): v is Info {
  if (!isObject(v) || !isStr(v.version) || !isStr(v.state) || !isBool(v.generating)) return false;
  const m = v.model;
  return m === null || (isObject(m) && isStr(m.path) && isStr(m.name) && isBool(m.streaming));
}

export function isConfig(v: unknown): v is Config {
  return isObject(v) && isObject(v.values) && Array.isArray(v.user_keys) && isBool(v.valid);
}

export function isParamSpecList(v: unknown): v is ParamSpec[] {
  return Array.isArray(v) && v.every((p) => isObject(p) && isStr(p.key) && isStr(p.type) && isStr(p.group));
}

export function isModelsResponse(v: unknown): v is ModelsResponse {
  return (
    isObject(v) &&
    isStr(v.models_dir) &&
    Array.isArray(v.local) &&
    v.local.every((m) => isObject(m) && isStr(m.name) && isStr(m.path)) &&
    Array.isArray(v.catalog) &&
    v.catalog.every((e) => isObject(e) && isStr(e.id) && isStr(e.title))
  );
}

export function isPlan(v: unknown): v is Plan {
  if (!isObject(v) || !isBool(v.available)) return false;
  // Available but with nothing to plan carries only `error`; a real plan carries its decisions.
  return !v.available || isStr(v.error) || Array.isArray(v.decisions);
}

export function isDecideResult(v: unknown): v is DecideResult {
  return isObject(v) && isBool(v.cancelled) && isNum(v.best) && Array.isArray(v.choice_logp);
}

export function isTokenEvent(v: unknown): v is TokenEvent {
  return isObject(v) && isNum(v.step) && isNum(v.wall_ms);
}

export function isDoneSummary(v: unknown): v is DoneSummary {
  return isObject(v) && isBool(v.cancelled) && isNum(v.tokens) && isNum(v.tok_s);
}

export function isDownloadEvent(v: unknown): v is DownloadEvent {
  return isObject(v) && isStr(v.id) && isStr(v.state);
}
