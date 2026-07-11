package io.bigmoeonedge.example

/**
 * Hides a reasoning model's internal "thinking" from the displayed answer, keeping only the
 * final response. This is a display-only concern: the engine streams the raw tokens verbatim
 * (it stays model-agnostic and never edits output), and the UI decides what to show.
 *
 * There is no metadata that declares where a model's reasoning starts and ends, so a fully
 * generic filter is not possible — the delimiters are a model-family convention. We keep the
 * known conventions in one small list here; add a pair when a model uses a new one:
 *   - Harmony-style channels (`<|channel>… <channel|>`) — e.g. this Gemma 4 build's template.
 *   - `<think>…</think>` — Qwen3 / DeepSeek-R1 style.
 *
 * Streaming-aware: while a block is still open (close marker not generated yet) everything from
 * the open marker on is hidden, so the reasoning never flashes into view mid-generation.
 */
object ReasoningFilter {
    private val PAIRS = listOf(
        "<|channel>" to "<channel|>",
        "<think>" to "</think>",
    )

    fun visible(raw: String): String {
        var s = raw
        for ((open, close) in PAIRS) {
            while (true) {
                val a = s.indexOf(open)
                if (a < 0) break
                val b = s.indexOf(close, a + open.length)
                s = if (b < 0) {
                    s.substring(0, a) // block still open (mid-stream) — hide from here
                } else {
                    s.substring(0, a) + s.substring(b + close.length)
                }
            }
        }
        return s.trim()
    }
}
