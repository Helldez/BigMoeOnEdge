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
    label: spec?.label ?? a.param,
    href: settingHref(a.param),
  };
}
