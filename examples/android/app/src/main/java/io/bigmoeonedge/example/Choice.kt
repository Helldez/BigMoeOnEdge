package io.bigmoeonedge.example

import org.json.JSONObject
import kotlin.math.exp

/** One option of a Choose turn and the probability the model put on it. */
data class ChoiceScore(val label: String, val text: String, val prob: Double)

/**
 * The Choose mode: the model picks one of the user's options instead of writing an answer. It is
 * the engine's decide request (docs/decide.md): one prefill and no decode, so on a model streamed
 * from flash the answer costs what reading the prompt costs and nothing more.
 *
 * Everything that turns the user's input into that request, and the engine's reply back into
 * something to show, lives here, so the service and the screen never spell out the format.
 */
object Choice {
    /** Options are labelled A, B, C…: single letters, one token each in the usual tokenizers. Should two
     *  ever share a first token, the engine refuses the request rather than answer with a tie. */
    const val MAX_OPTIONS = 26

    fun labels(n: Int): List<String> = (0 until n.coerceAtMost(MAX_OPTIONS)).map { ('A' + it).toString() }

    /** The user's options, one per non-blank line. */
    fun parseOptions(text: String): List<String> =
        text.lines().map { it.trim() }.filter { it.isNotEmpty() }.take(MAX_OPTIONS)

    /**
     * The part of the prompt that changes from one question to the next: the lettered options and
     * the instruction to answer with a letter. The question itself is the request's prefix, so
     * asking it again with other options restores it instead of reading it again.
     */
    fun suffix(options: List<String>): String = buildString {
        append("\n\nOptions:\n")
        labels(options.size).zip(options).forEach { (l, o) -> append(l).append(") ").append(o).append('\n') }
        append("Answer with the letter of one option only.")
    }

    /** How the user's turn reads in the transcript. */
    fun userText(question: String, options: List<String>): String = buildString {
        append(question)
        labels(options.size).zip(options).forEach { (l, o) -> append('\n').append(l).append(") ").append(o) }
    }

    /** The engine's BMOE_DECIDE line, read back against the options it was asked about. */
    class Result(val scores: List<ChoiceScore>, val best: Int, val metrics: String, val cancelled: Boolean)

    fun parse(json: String, options: List<String>): Result {
        val o = JSONObject(json)
        val logp = o.optJSONArray("choice_logp")
        val labels = labels(options.size)
        // exp(log p): the model's probability over its whole vocabulary, so the options need not sum
        // to one. What is missing is mass the model put on something else, and is worth seeing.
        val scores = labels.indices.map { i ->
            val lp = logp?.optDouble(i, Double.NEGATIVE_INFINITY) ?: Double.NEGATIVE_INFINITY
            ChoiceScore(labels[i], options[i], if (lp.isNaN()) 0.0 else exp(lp))
        }
        val loc = java.util.Locale.US
        val metrics = buildString {
            append(String.format(loc, "prefill %.1fs", o.optDouble("prefill_s")))
            append(String.format(loc, " (%d tok", o.optInt("n_prefilled")))
            val reused = o.optInt("n_reused")
            if (reused > 0) append(String.format(loc, ", %d reused", reused))
            append(")")
            val stateMib = o.optDouble("prefix_state_mib", 0.0)
            if (stateMib > 0) append(String.format(loc, " · context kept %.0f MiB", stateMib))
        }
        return Result(scores, o.optInt("best", -1), metrics, o.optBoolean("cancelled"))
    }
}
