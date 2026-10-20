package io.bigmoeonedge.app.data

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import java.io.File

/**
 * The SoC temperature while generating, read on the app's side: the kernel thermal zones
 * (`/sys/class/thermal/thermal_zone*`) expose the on-die sensors that track compute load, a far
 * better proxy for streaming heat than the battery pack, which lags and reflects charging. No
 * permission is needed and the read is best effort: the CPU zones are found once by their `type`
 * ([CpuZones]), and where none is readable (some vendors lock the node down) the battery
 * temperature stands in, so the figure never goes blank. It does not travel through the engine.
 */
class Thermal(private val ctx: Context) {
    private val cpu = CpuZones(File("/sys/class/thermal"))

    fun sampleCelsius(): Double? = cpu.sample() ?: readBattery()

    private fun readBattery(): Double? {
        val tenths = runCatching {
            ctx.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
                ?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE) ?: Int.MIN_VALUE
        }.getOrDefault(Int.MIN_VALUE)
        return if (tenths != Int.MIN_VALUE) tenths / 10.0 else null
    }
}

/**
 * The hottest CPU sensor among the thermal zones under [root]. Some kernels also publish a
 * threshold as a zone whose `type` names the CPU: its `temp` is a constant (95 °C on one phone)
 * that passes any sanity range. Two facts tell it from a sensor without naming any zone: it reads
 * one of its own `trip_point_*_temp` values, and it never moves. Zones are taken in their numeric
 * order, so the result does not depend on the order the directory lists them in (#311).
 */
internal class CpuZones(private val root: File) {
    private class Zone(val temp: File, val trips: Set<Double>) {
        var first: Double? = null
        var moved = false
    }

    @Volatile private var zones: List<Zone>? = null

    fun sample(): Double? {
        val all = zones ?: discover().also { zones = it }
        val read = all.mapNotNull { z -> read(z.temp)?.takeIf { it !in z.trips }?.let { z to it } }
        if (read.isEmpty()) return null
        for ((z, t) in read) {
            val first = z.first
            if (first == null) z.first = t else if (t != first) z.moved = true
        }
        // Before any zone has moved, a constant cannot be told from a sensor at rest yet: the median
        // keeps one outlier from becoming the figure.
        val moving = read.filter { it.first.moved }.map { it.second }
        return if (moving.isNotEmpty()) moving.max() else read.map { it.second }.sorted()[read.size / 2]
    }

    private fun discover(): List<Zone> {
        val dirs = root.listFiles { f -> f.name.startsWith("thermal_zone") } ?: return emptyList()
        return dirs
            .sortedBy { it.name.removePrefix("thermal_zone").toIntOrNull() ?: Int.MAX_VALUE }
            .filter { z ->
                runCatching { File(z, "type").readText().trim().contains("cpu", ignoreCase = true) }.getOrDefault(false)
            }
            .map { z ->
                val trips = z.listFiles { f -> f.name.startsWith("trip_point_") && f.name.endsWith("_temp") }
                    ?.mapNotNull { read(it) }?.toSet().orEmpty()
                Zone(File(z, "temp"), trips)
            }
    }

    private fun read(f: File): Double? {
        val raw = runCatching { f.readText().trim().toDouble() }.getOrNull() ?: return null
        return normalise(raw).takeIf { it in 1.0..150.0 }
    }

    /** Kernel thermal `temp` is conventionally millidegrees, but some vendors report tenths or degrees. */
    private fun normalise(raw: Double): Double = when {
        raw > 1000.0 -> raw / 1000.0
        raw > 200.0 -> raw / 10.0
        else -> raw
    }
}
