package io.bigmoeonedge.app.engine

import org.json.JSONArray
import org.json.JSONObject

/**
 * One row of the engine's parameter table, as `GET /api/params` describes it. The settings screen
 * is rendered from these and nothing else: it knows no parameter by name, so a knob added to the
 * engine appears here with no app change. Same schema the web UI renders from.
 */
data class ParamSpec(
    val key: String,
    val label: String,
    val type: String, // bool | int | float | choice | path | text
    val group: String,
    val groupLabel: String,
    val level: String, // basic | advanced | experimental | debug
    val scope: String, // session | request
    val help: String,
    val flag: String,
    val min: Double?,
    val max: Double?,
    val unit: String?,
    val acceptsAuto: Boolean,
    val lossy: Boolean,
    val choices: List<Pair<String, String>>, // value to label
    val default: Any?,
) {
    companion object {
        val LEVELS = listOf("basic", "advanced", "experimental", "debug")

        fun parse(arr: JSONArray): List<ParamSpec> = (0 until arr.length()).map { i -> of(arr.getJSONObject(i)) }

        fun of(o: JSONObject): ParamSpec {
            val choices = o.optJSONArray("choices")?.let { c ->
                (0 until c.length()).map { i ->
                    val x = c.getJSONObject(i)
                    x.optString("value") to x.optString("label").ifEmpty { x.optString("value") }
                }
            }.orEmpty()
            return ParamSpec(
                key = o.getString("key"),
                label = o.optString("label", o.getString("key")),
                type = o.optString("type", "text"),
                group = o.optString("group", ""),
                groupLabel = o.optString("group_label", o.optString("group", "")),
                level = o.optString("level", "basic"),
                scope = o.optString("scope", "session"),
                help = o.optString("help", ""),
                flag = o.optString("flag", ""),
                min = if (o.has("min") && !o.isNull("min")) o.getDouble("min") else null,
                max = if (o.has("max") && !o.isNull("max")) o.getDouble("max") else null,
                unit = o.optString("unit").takeIf { it.isNotEmpty() },
                acceptsAuto = o.optBoolean("accepts_auto", false),
                lossy = o.optBoolean("lossy", false),
                choices = choices,
                default = if (o.has("default") && !o.isNull("default")) o.get("default") else null,
            )
        }

        /** Groups in the table's order, each with its rows in the table's order. */
        fun grouped(specs: List<ParamSpec>): List<Pair<String, List<ParamSpec>>> {
            val order = mutableListOf<String>()
            val by = mutableMapOf<String, MutableList<ParamSpec>>()
            for (s in specs) {
                if (s.group !in by) {
                    order += s.group
                    by[s.group] = mutableListOf()
                }
                by.getValue(s.group) += s
            }
            return order.map { g -> (by.getValue(g).first().groupLabel) to by.getValue(g) }
        }

        /** Whether a row at [level] is shown when the filter admits up to [shown]. */
        fun visible(level: String, shown: String): Boolean =
            LEVELS.indexOf(level).coerceAtLeast(0) <= LEVELS.indexOf(shown).coerceAtLeast(0)
    }
}

/** A value as the config carries it, rendered for a field: numbers plain, `auto` as is, bools as is. */
fun paramValueText(v: Any?): String = when (v) {
    null -> ""
    is Boolean -> v.toString()
    is Number -> if (v is Double && v == Math.floor(v) && !v.isInfinite()) v.toLong().toString() else v.toString()
    else -> v.toString()
}

/**
 * What a field's text becomes on the wire: the JSON type the schema says (`bool`, `int`, `float`)
 * or a string (`choice`, `path`, `text`, and the literal `auto` where the row accepts it). Null when
 * the text is not a value of that type: the field shows the problem and nothing is sent.
 */
fun paramValueFromText(spec: ParamSpec, text: String): Any? {
    val t = text.trim()
    return when (spec.type) {
        "bool" -> t.toBooleanStrictOrNull()
        "int" -> if (spec.acceptsAuto && t == "auto") "auto" else t.toLongOrNull()
        "float" -> t.toDoubleOrNull()
        else -> t
    }
}
