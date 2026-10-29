package io.bigmoeonedge.app.engine

/**
 * The Plan screen's figures, worked out from what `GET /api/plan` and the parameter table say:
 * decode expected against measured, the cache hit rate credited against measured, the memory held
 * against what the process may hold, and what Apply changed. Nothing here names a
 * device or a knob. Same rules as the web UI (ui/src/lib/plan.ts).
 */
data class PlanFigures(
    val predictedTokS: Double?,
    val measuredTokS: Double?,
    /** Measured against predicted, as bmoe-cli prints it: positive when the run beat the plan. */
    val deltaPct: Double?,
    /** The hit rate the cost model credited the cache with, and the one the last turn measured. */
    val creditedHitPct: Double?,
    val measuredHitPct: Double?,
    /** The pool of what this process may hold, when the plan charged one. */
    val heldBytes: Long?,
    val capBytes: Long?,
)

fun planFigures(p: PlanResult): PlanFigures {
    val ps = p.predictedSPerToken.takeIf { it > 0 }
    val ms = p.measured?.sPerToken?.takeIf { it > 0 }
    val host = p.pools.firstOrNull { it.name == "host" } ?: p.pools.firstOrNull()
    return PlanFigures(
        predictedTokS = ps?.let { 1 / it },
        measuredTokS = ms?.let { 1 / it },
        deltaPct = if (ps != null && ms != null) 100 * (ps / ms - 1) else null,
        creditedHitPct = p.predictedHitPct.takeIf { it >= 0 },
        measuredHitPct = p.measured?.cacheHitPct?.takeIf { it >= 0 },
        heldBytes = host?.takeIf { it.capBytes > 0 }?.heldBytes,
        capBytes = host?.capBytes?.takeIf { it > 0 },
    )
}

/** One row Apply changed: its settings label (the key where the table has no row for it), from and to. */
data class AppliedChange(val key: String, val label: String, val from: String, val to: String)

/** A value as a person reads it: a special value by its name, a choice by its label, a number with its unit. */
fun shownValue(spec: ParamSpec?, v: Any?): String {
    if (v == null) return ""
    val named = spec?.specialLabel(v)
    val choice = spec?.choices?.firstOrNull { it.first == paramValueText(v) }?.second
    val text = named ?: choice ?: paramValueText(v)
    return if (spec?.unit != null && v is Number && named == null) "$text ${spec.unit}" else text
}

/** What applying a plan changed, row by row in table order (keys the table lacks after), worded from the table. */
fun appliedChanges(before: Map<String, Any?>, after: Map<String, Any?>, params: List<ParamSpec>): List<AppliedChange> {
    val known = params.map { it.key }.toSet()
    val keys = params.map { it.key } + after.keys.filter { it !in known }
    return keys.filter { it in after && !sameValue(before[it], after[it]) }.map { key ->
        val spec = params.firstOrNull { it.key == key }
        AppliedChange(key, spec?.label?.ifEmpty { null } ?: key, shownValue(spec, before[key]), shownValue(spec, after[key]))
    }
}

/** What "Copy diagnostic" puts on the clipboard: the plan's command and the planner's whole account. */
fun diagnosticText(args: List<String>, explain: String): String {
    val cmd = if (args.isNotEmpty()) "bmoe-cli " + args.joinToString(" ") else "(no arguments: the plan declines streaming)"
    return if (explain.isNotBlank()) "$cmd\n\n${explain.trim()}\n" else "$cmd\n"
}

/**
 * The model file the Plan screen is for, by its base name: the plan's own (`model`, the path it
 * measured for) while a plan is shown, else what a load would open (the first read-only row with a
 * value, the one Models sets), else the loaded model; "" with none. A plan is for one model, so the
 * screen says which before anyone taps Measure. Same rule as the web UI.
 */
fun planModelName(plan: PlanResult?, params: List<ParamSpec>?, cfg: ConfigState?, loadedPath: String?): String {
    val planned = plan?.takeIf { it.available && !it.none && it.error.isEmpty() }?.model.orEmpty()
    val target = cfg?.let { c ->
        params.orEmpty().filter { it.groupReadOnly }.map { paramValueText(c.value(it.key)) }.firstOrNull { it.isNotEmpty() }
    }.orEmpty()
    val path = planned.ifEmpty { target }.ifEmpty { loadedPath.orEmpty() }
    return path.substringAfterLast('/').substringAfterLast('\\')
}
