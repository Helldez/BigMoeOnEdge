// What the screens say about the loaded session's context against what a client of the server
// sends. The recommendation, whether the context is short of it and the parameter that raises it
// all come from the server (`model.ctx_advice`, docs/server-api.md): nothing here names a number
// or a parameter key.
import { settingHref } from './route';
import type { ModelInfo, ParamSpec } from './types';

export interface ContextAdvice {
  /** The loaded session's context, in tokens. */
  nCtx: number;
  /** What a coding agent's first request needs, with room for its answer. */
  recommended: number;
  /** The context is below the recommendation: a client like that will be refused. */
  short: boolean;
  /** A request was refused for the context since the load: the warning says so. */
  refused: boolean;
  /** Short, and the server serves the network or has refused a request: the status line warns. */
  warn: boolean;
  /** Which warning a dismissal is for: another model, another context or a first refusal brings it back. */
  dismissKey: string;
  /** The setting that raises it, as the parameter table labels it. */
  label: string;
  /** Where that setting is, in this UI. */
  href: string;
}

/** The advice for `model`, or null when there is no model or the server sent none. */
export function contextAdvice(model: ModelInfo | null | undefined, params: readonly ParamSpec[] | null | undefined): ContextAdvice | null {
  const a = model?.ctx_advice;
  if (!model || !a || typeof a.recommended !== 'number' || typeof a.param !== 'string' || !a.param) return null;
  const spec = params?.find((p) => p.key === a.param);
  return {
    nCtx: model.n_ctx,
    recommended: a.recommended,
    short: a.short === true,
    refused: a.refused === true,
    warn: a.warn === true,
    dismissKey: `${model.path}\n${model.n_ctx}${a.refused === true ? '\nrefused' : ''}`,
    label: spec?.label ?? a.param,
    href: settingHref(a.param),
  };
}

/** The status line's warning: what happened or what will, then the figures. */
export function warningText(a: ContextAdvice): string {
  const cost = 'Raising it takes memory from the expert cache.';
  return a.refused
    ? `A request was refused: it did not fit the context of ${a.nCtx} tokens. Coding agents need ${a.recommended}. ${cost}`
    : `Coding agents need a larger context: ${a.recommended} tokens, this model has ${a.nCtx}. ${cost}`;
}

const DISMISSED_KEY = 'bmoe.ctxWarningDismissed';

/** The load whose warning this browser dismissed, or "" (storage can be missing or refuse). */
export function dismissedContextWarning(storage: Pick<Storage, 'getItem'> | undefined = globalThis.localStorage): string {
  try {
    return storage?.getItem(DISMISSED_KEY) ?? '';
  } catch {
    return '';
  }
}

/** Remember that the warning for `key` was dismissed; one load at a time is enough. */
export function dismissContextWarning(key: string, storage: Pick<Storage, 'setItem'> | undefined = globalThis.localStorage): void {
  try {
    storage?.setItem(DISMISSED_KEY, key);
  } catch {
    // Not remembered: the warning comes back on the next visit, which is all that is lost.
  }
}
