package io.bigmoeonedge.app.engine

/**
 * What the screens say about the loaded session's context against what a client of the server
 * sends. The recommendation, whether the context is short of it and the parameter that raises it
 * all come from the server (`model.ctx_advice`); nothing here names a number or a parameter key.
 * Same rule as the web UI (ui/src/lib/context.ts).
 */
data class ContextWarning(
    /** The loaded session's context, in tokens. */
    val nCtx: Int,
    /** What a coding agent's first request needs, with room for its answer. */
    val recommended: Int,
    /** The context is below the recommendation: a client like that is refused. */
    val short: Boolean,
    /** A request was refused for the context since the load: the warning says so. */
    val refused: Boolean,
    /** Short, and the server serves the network or has refused a request: the status line warns. */
    val warn: Boolean,
    /** Which warning a dismissal is for: another model, another context or a first refusal brings it back. */
    val dismissKey: String,
    /** The setting that raises it, as the parameter table labels it. */
    val label: String,
    /** That setting's key, for the link that lands on it in Settings. */
    val key: String,
)

/** The advice for [model], or null when there is no model or the server sent none. */
fun contextWarning(model: ModelInfo?, params: List<ParamSpec>?): ContextWarning? {
    val a = model?.ctxAdvice ?: return null
    val label = params?.firstOrNull { it.key == a.param }?.label ?: a.param
    val dismissKey = "${model.path}\n${model.nCtx}" + if (a.refused) "\nrefused" else ""
    return ContextWarning(model.nCtx, a.recommended, a.short, a.refused, a.warn, dismissKey, label, a.param)
}

/** The warning's text: what happened or what will, then the figures (same as the web UI's). */
fun ContextWarning.text(): String {
    val cost = "Raising it takes memory from the expert cache."
    return if (refused) "A request was refused: it did not fit the context of $nCtx tokens. Coding agents need $recommended. $cost"
    else "Coding agents need a larger context: $recommended tokens, this model has $nCtx. $cost"
}
