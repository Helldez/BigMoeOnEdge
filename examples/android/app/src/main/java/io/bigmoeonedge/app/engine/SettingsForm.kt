package io.bigmoeonedge.app.engine

/**
 * The settings form as docs/screens.md ("Settings") describes it, worked out from the parameter
 * table alone: one card per group in the table's group order, each listing the rows the table marks
 * for a form (`form`) in the table's order, and where each value came from.
 * Nothing here names a group or a parameter. Same rules as the web UI (ui/src/lib/schema.ts).
 */

/** One settings card: a group of the table and the rows of it the form shows, in table order. */
data class SettingsCard(
    val group: String,
    val label: String,
    val help: String,
    /** Shown, not edited: a front-end sets it with a control of its own. */
    val readOnly: Boolean,
    val params: List<ParamSpec>,
)

/**
 * The cards for [params]: the rows the table offers a form, and the rows in [pinned] whatever the
 * table says (a value the person set is never hidden, so it can be seen and reset; a row a link
 * points at is shown so the link lands). A card left with no row is dropped.
 */
fun settingsCards(params: List<ParamSpec>, pinned: Set<String> = emptySet()): List<SettingsCard> {
    return ParamSpec.grouped(params).mapNotNull { (_, rows) ->
        val first = rows.first()
        val shown = rows.filter { it.form || it.key in pinned }
        if (shown.isEmpty()) return@mapNotNull null
        SettingsCard(
            group = first.group,
            label = first.groupLabel.ifEmpty { first.group },
            help = first.groupHelp,
            readOnly = first.groupReadOnly,
            params = shown,
        )
    }
}

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

/** The rows shown whether or not the table offers them a form: the ones the person set. */
fun pinnedKeys(params: List<ParamSpec>, cfg: ConfigState): Set<String> =
    params.filter { it.key in cfg.userKeys }.map { it.key }.toSet()
