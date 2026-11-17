package io.bigmoeonedge.app.data

import kotlin.math.roundToLong

/**
 * How fast a download goes and how long it has left, from the byte counts reported and when. Over a
 * sliding window, so a stall or a burst moves the estimate without making it jump. The web UI's
 * rules (ui/src/lib/eta.ts).
 */
object Eta {
    /** One reading: [atMs] on any monotonic clock, [received] bytes so far. */
    data class Sample(val atMs: Long, val received: Long)

    const val WINDOW_MS = 20_000L

    /** Keep the samples of the last [windowMs], and none with more bytes than [next] (a restart), plus [next]. */
    fun addSample(samples: List<Sample>, next: Sample, windowMs: Long = WINDOW_MS): List<Sample> =
        samples.filter { next.atMs - it.atMs <= windowMs && it.received <= next.received } + next

    /** Bytes per second over the window, or null until two samples a second apart exist. */
    fun rate(samples: List<Sample>): Double? {
        if (samples.size < 2) return null
        val a = samples.first()
        val b = samples.last()
        val dt = (b.atMs - a.atMs) / 1000.0
        if (dt < 1) return null
        return maxOf(0.0, (b.received - a.received) / dt)
    }

    /** Seconds left at the current rate, or null when unknown. */
    fun secondsLeft(samples: List<Sample>, total: Long): Double? {
        val r = rate(samples)
        val last = samples.lastOrNull()
        if (r == null || r == 0.0 || last == null || total <= 0) return null
        return maxOf(0.0, (total - last.received) / r)
    }

    /** "about 12 min", "about 1 h 20 min", "less than a minute": a person's rounding. */
    fun humanDuration(s: Double): String {
        if (s < 60) return "less than a minute"
        val min = (s / 60).roundToLong()
        if (min < 60) return "about $min min"
        val h = min / 60
        val m = min % 60
        return if (m > 0) "about $h h $m min" else "about $h h"
    }
}
