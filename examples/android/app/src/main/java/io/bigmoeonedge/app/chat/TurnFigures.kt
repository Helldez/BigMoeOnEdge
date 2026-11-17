package io.bigmoeonedge.app.chat

import org.json.JSONObject
import java.util.Locale
import kotlin.math.roundToLong

/** One figure of a finished answer: what it is, its value, and what it measures in plain words. */
data class TurnFigure(val id: String, val label: String, val value: String, val hint: String)

/** One part of a token's time: [key] is compute, disk, mgmt or other; [pct] a whole percentage. */
data class TimeShare(val key: String, val pct: Int)

/** A finished answer's run summary (the engine's `done` event) and the context the model had then. */
data class TurnSummary(val summary: JSONObject, val nCtx: Int)

/**
 * The figures of a finished answer, from the turn's own summary, in the words every other screen uses
 * for the same quantity; the one quiet line under the answer (`tg … · pp …`) and the list it opens to.
 * The same rules as the web UI (ui/src/lib/turnMetrics.ts, ui/src/components/chat/TurnFigures.svelte).
 */
object TurnFigures {
    private val US = Locale.US

    /** A value longer than this goes on its own line under its label; a shorter one sits at the label's right. */
    const val INLINE_VALUE_MAX = 24

    private fun f1(v: Double) = String.format(US, "%.1f", v)

    /** The prompt-reading rate: the engine's, or the tokens read over the time it took; 0 when unknown. */
    fun prefillRate(s: JSONObject): Double {
        val tps = s.optDouble("prefill_tps", 0.0)
        if (tps > 0) return tps
        val n = s.optInt("n_prompt", -1)
        val secs = s.optDouble("prefill_s", 0.0)
        return if (n > 0 && secs > 0) n / secs else 0.0
    }

    /** The one line under a finished answer: the writing speed, then the reading speed when known. */
    fun line(s: JSONObject): String {
        val tg = live(s.optDouble("tok_s", 0.0))
        val pp = prefillRate(s)
        return if (pp > 0) "$tg · pp ${String.format(US, "%.0f", pp)} tok/s" else tg
    }

    /**
     * The live writing speed smoothed over the tokens so far (exponential, [alpha] 0.2 as the web
     * UI's): [prev] null starts from the sample.
     */
    fun ema(prev: Double?, sample: Double, alpha: Double = 0.2): Double =
        if (prev == null || !prev.isFinite()) sample else prev + alpha * (sample - prev)

    /** The writing speed while an answer streams, where the finished line will land. */
    fun live(tokS: Double): String = "tg ${f1(tokS)} tok/s"

    /**
     * Where each token's time went, as whole percentages that add up to exactly 100 (largest
     * remainder): computing, waiting for the disk, managing the cache, and what no measured term
     * claims. Parts that round to nothing are left out. Empty when the turn has no rate.
     */
    fun timeShares(s: JSONObject): List<TimeShare> {
        val tokS = s.optDouble("tok_s", 0.0)
        if (!(tokS > 0)) return emptyList()
        val wall = 1 / tokS
        val compute = maxOf(0.0, s.optDouble("compute_s_tok", 0.0))
        val disk = maxOf(0.0, s.optDouble("flash_wait_s_tok", 0.0))
        val mgmt = maxOf(0.0, s.optDouble("mgmt_s_tok", 0.0))
        val other = maxOf(0.0, wall - compute - disk - mgmt)
        val parts = listOf("compute" to compute, "disk" to disk, "mgmt" to mgmt, "other" to other)
        return largestRemainder(parts.map { it.second }).let { pct ->
            parts.mapIndexed { i, (key, _) -> TimeShare(key, pct[i]) }.filter { it.pct > 0 }
        }
    }

    /**
     * [values] as whole percentages of their sum that add up to exactly 100: each rounded down, the
     * points left over handed to the largest remainders. All zeros when the sum is not positive.
     */
    fun largestRemainder(values: List<Double>): List<Int> {
        val total = values.sum()
        if (!(total > 0)) return values.map { 0 }
        val exact = values.map { 100 * it / total }
        val pct = exact.map { Math.floor(it).toInt() }.toMutableList()
        var left = 100 - pct.sum()
        for (i in exact.indices.sortedByDescending { exact[it] - Math.floor(exact[it]) }) {
            if (left <= 0) break
            pct[i]++
            left--
        }
        return pct
    }

    private fun shareText(p: TimeShare) = when (p.key) {
        "compute" -> "computing ${p.pct}%"
        "disk" -> "waiting for the disk ${p.pct}%"
        "mgmt" -> "managing the cache ${p.pct}%"
        else -> "other ${p.pct}%"
    }

    /**
     * Every figure of the answer, [expectedTokS] being the rate the plan predicts (null when none).
     * Only the time split is a set of percentages, and it adds up to 100; what was found in memory is
     * a different quantity, so it is said as a count, never as a percentage beside the time shares.
     */
    fun of(s: JSONObject, nCtx: Int, expectedTokS: Double?): List<TurnFigure> {
        val out = mutableListOf<TurnFigure>()
        val tokS = s.optDouble("tok_s", 0.0)
        val expected = if (expectedTokS != null && expectedTokS > 0) ", plan expected ${f1(expectedTokS)}" else ""
        out += TurnFigure("speed", Words.SPEED, "${f1(tokS)} tok/s$expected", Words.SPEED_HINT)
        val shares = timeShares(s)
        if (shares.isNotEmpty()) out += TurnFigure("time", Words.TIME, shares.joinToString(" · ", transform = ::shareText), Words.TIME_HINT)
        val hit = s.optDouble("cache_hit_pct", -1.0)
        if (hit >= 0) out += TurnFigure("cache", Words.CACHE, "${hit.roundToLong()} of every 100 parts", Words.CACHE_HINT)
        val prefillS = s.optDouble("prefill_s", 0.0)
        if (prefillS > 0) {
            // The tokens this turn read: with the conversation's leading tokens reused from the KV
            // cache, that is the new message, not the whole history.
            val n = maxOf(0, s.optInt("n_prompt", 0))
            out += TurnFigure("prompt", Words.PROMPT, "${f1(prefillRate(s))} tok/s, $n tokens in ${f1(prefillS)} s", Words.PROMPT_HINT)
        }
        val nPast = s.optInt("n_past", -1)
        if (nPast >= 0 && nCtx > 0) {
            out += TurnFigure("context", Words.CONTEXT, String.format(US, "%,d of %,d tokens", nPast, nCtx), Words.CONTEXT_HINT)
        }
        val drafted = s.optLong("mtp_drafted", 0)
        if (s.optLong("mtp_decodes", 0) > 0 && drafted > 0) {
            out += TurnFigure("drafts", Words.DRAFTS, "${s.optLong("mtp_accepted", 0)} of $drafted guesses", Words.DRAFTS_HINT)
        }
        if (s.optBoolean("cancelled", false)) out += TurnFigure("stopped", Words.STOPPED, "", Words.STOPPED_HINT)
        return out
    }
}
