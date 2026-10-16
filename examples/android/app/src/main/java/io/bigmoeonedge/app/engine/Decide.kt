package io.bigmoeonedge.app.engine

import org.json.JSONArray
import org.json.JSONObject
import java.util.Locale
import kotlin.math.exp

/**
 * Choose from options: the engine's decide request (docs/decide.md) as the chat offers it,
 * through `POST /api/decide`. The options are lettered, the question is the request's prefix and
 * the lettered options its suffix, and the answer is each option with the probability the model
 * put on it. Pure, so the wording of the prompt and the reading of the answer are tested without
 * a server; the web UI builds the same request, so a decision reads alike on both.
 */
object Decide {
    /** Single letters, one token each in the usual tokenizers: two options never share a first token. */
    const val MAX_OPTIONS = 26

    fun labels(n: Int): List<String> = (0 until n.coerceAtMost(MAX_OPTIONS)).map { ('A' + it).toString() }

    /** The options, one per non-blank line. */
    fun parseOptions(text: String): List<String> =
        text.lines().map { it.trim() }.filter { it.isNotEmpty() }.take(MAX_OPTIONS)

    /**
     * The part of the prompt that changes from one question to the next: the lettered options and
     * the instruction to answer with a letter. The question is the prefix, so asking it again with
     * other options restores it instead of reading it again.
     */
    fun suffix(options: List<String>): String = buildString {
        append("\n\nOptions:\n")
        labels(options.size).zip(options).forEach { (l, o) -> append(l).append(") ").append(o).append('\n') }
        append("Answer with the letter of one option only.")
    }

    /** How the question reads in the transcript. */
    fun userText(question: String, options: List<String>): String = buildString {
        append(question)
        labels(options.size).zip(options).forEach { (l, o) -> append('\n').append(l).append(") ").append(o) }
    }

    fun requestBody(question: String, options: List<String>): JSONObject =
        JSONObject().put("prefix", question).put("suffix", suffix(options)).put("choices", JSONArray(labels(options.size)))

    /** The answer, read back against the options it was asked about. */
    class Result(val scores: List<ChoiceScore>, val best: Int, val metrics: String, val cancelled: Boolean)

    fun parse(o: JSONObject, options: List<String>): Result {
        val logp = o.optJSONArray("choice_logp")
        val ls = labels(options.size)
        // exp(log p): the model's probability over its whole vocabulary, so the options need not sum
        // to one. What is missing is mass the model put on something else, and is worth seeing.
        val scores = ls.indices.map { i ->
            val lp = if (logp == null || logp.isNull(i)) Double.NEGATIVE_INFINITY else logp.optDouble(i, Double.NEGATIVE_INFINITY)
            ChoiceScore(ls[i], options[i], if (lp.isNaN() || lp.isInfinite()) 0.0 else exp(lp))
        }
        val metrics = buildString {
            append(String.format(Locale.US, "prefill %.1fs", o.optDouble("prefill_s", 0.0)))
            append(String.format(Locale.US, " (%d tok", o.optInt("n_prefilled")))
            val reused = o.optInt("n_reused")
            if (reused > 0) append(String.format(Locale.US, ", %d reused", reused))
            append(")")
            val stateMib = o.optDouble("prefix_state_mib", 0.0)
            if (stateMib > 0) append(String.format(Locale.US, " · context kept %.0f MiB", stateMib))
        }
        return Result(scores, o.optInt("best", -1), metrics, o.optBoolean("cancelled"))
    }
}
