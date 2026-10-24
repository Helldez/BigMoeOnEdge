package io.bigmoeonedge.app.engine

/**
 * The settings form as docs/screens.md ("Settings") describes it, worked out from the parameter
 * table alone: one card per group in the table's group order, the basic rows open and the deeper
 * levels folded inside each card, a level filter and a search, and where each value came from.
 * Nothing here names a group or a parameter. Same rules as the web UI (ui/src/lib/schema.ts).
 */

/** The rows of one level a card folds away. */
data class LevelRows(val level: String, val params: List<ParamSpec>)

/** One settings card: a group of the table, its basic rows open and the rest folded by level. */
data class SettingsCard(
    val group: String,
    val label: String,
    val help: String,
    /** Shown, not edited: a front-end sets it with a control of its own. */
    val readOnly: Boolean,
    val basic: List<ParamSpec>,
    val more: List<LevelRows>,
    /** Rows of this group the level filter leaves out. */
    val hidden: Int,
) {
    val moreCount: Int get() = more.sumOf { it.params.size }
}

/**
 * The cards for [params]: rows up to level [upTo], every level when [query] holds words (only the
 * matching rows then), and the rows in [pinned] whatever the filter (a value someone or the plan set
 * is never hidden). A card left with no row is dropped.
 */
fun settingsCards(params: List<ParamSpec>, upTo: String, query: String = "", pinned: Set<String> = emptySet()): List<SettingsCard> {
    val words = query.lowercase().split(Regex("\\s+")).filter { it.isNotEmpty() }
    val limit = ParamSpec.levelRank(upTo)
    return ParamSpec.grouped(params).mapNotNull { (_, rows) ->
        val first = rows.first()
        var hidden = 0
        val basic = mutableListOf<ParamSpec>()
        val more = linkedMapOf<String, MutableList<ParamSpec>>()
        for (p in rows) {
            val shown = if (words.isNotEmpty()) matches(p, words)
            else p.level == "basic" || ParamSpec.levelRank(p.level) <= limit || p.key in pinned
            if (!shown) {
                if (words.isEmpty()) hidden++
                continue
            }
            if (p.level == "basic") basic += p else more.getOrPut(p.level) { mutableListOf() } += p
        }
        if (basic.isEmpty() && more.isEmpty()) return@mapNotNull null
        SettingsCard(
            group = first.group,
            label = first.groupLabel.ifEmpty { first.group },
            help = first.groupHelp,
            readOnly = first.groupReadOnly,
            basic = basic,
            more = more.map { (l, ps) -> LevelRows(l, ps) }.sortedBy { ParamSpec.levelRank(it.level) },
            hidden = hidden,
        )
    }
}

private fun matches(p: ParamSpec, words: List<String>): Boolean {
    val hay = "${p.label} ${p.help} ${p.flag} ${p.key} ${p.groupLabel}".lowercase()
    return words.all { it in hay }
}

/** How many rows each level holds, for the filter's counts. */
fun levelCounts(params: List<ParamSpec>): Map<String, Int> = params.groupingBy { it.level }.eachCount()

/**
 * What "reset all my values" removes: the person's own values, never a key the server was launched
 * with and never a row of a read-only group (the model a load chose), which the form could not set back.
 */
fun resettableKeys(userKeys: Collection<String>, launchKeys: Set<String>, params: List<ParamSpec>): List<String> {
    val readOnly = params.filter { it.groupReadOnly }.map { it.key }.toSet()
    return userKeys.filter { it !in launchKeys && it !in readOnly }
}

/** Where a value came from, in the one chip a row carries. */
enum class ValueSource(val chip: String) { YOU("set by you"), PLAN("from the plan"), DEFAULT("default") }

private fun PlanDecision.isSet() = group.ifEmpty { "set" } == "set"

/** The person's value wins; else a plan that decided the row; else the server's default. */
fun valueSource(userSet: Boolean, decision: PlanDecision?): ValueSource = when {
    userSet -> ValueSource.YOU
    decision != null && decision.isSet() -> ValueSource.PLAN
    else -> ValueSource.DEFAULT
}

/** True when the plan decided this row and the value in force is not the one it decided. */
fun differsFromPlan(value: Any?, decision: PlanDecision?): Boolean {
    if (decision == null || !decision.isSet() || value == null) return false
    return !(sameValue(value, decision.value) || paramValueText(value) == decision.value)
}

/** Value equality across the JSON types a parameter can take ("auto" against 4096, 8 against "8", 8 against 8.0). */
fun sameValue(a: Any?, b: Any?): Boolean {
    if (a == b) return true
    if (a == null || b == null) return false
    fun num(x: Any): Double? = when (x) {
        is Number -> x.toDouble()
        is String -> x.trim().takeIf { it.isNotEmpty() }?.toDoubleOrNull()
        else -> null
    }
    if (a is Number || b is Number) {
        val x = num(a)
        val y = num(b)
        return x != null && y != null && x == y
    }
    return false
}

fun ParamSpec.isDefault(value: Any?): Boolean = value == null || sameValue(default, value)

/**
 * Whether the configuration names what a load opens: a row of a read-only group (the one a client
 * sets with a control of its own, from Models) holds a value. Read from the table, so no key is named.
 */
fun loadTargetChosen(params: List<ParamSpec>?, cfg: ConfigState?): Boolean =
    cfg != null && params.orEmpty().any { it.groupReadOnly && paramValueText(cfg.value(it.key)).isNotEmpty() }

/** The rows shown whatever the filter: set by the person, decided by the plan, or off their default. */
fun pinnedKeys(params: List<ParamSpec>, cfg: ConfigState): Set<String> {
    val decided = cfg.plan.orEmpty().map { it.knob }.toSet()
    return params.filter { it.key in cfg.userKeys || it.key in decided || !it.isDefault(cfg.value(it.key)) }.map { it.key }.toSet()
}
