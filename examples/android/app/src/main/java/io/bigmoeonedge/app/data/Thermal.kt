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
 * permission is needed and the read is best effort: the CPU zone is found once by its `type`, and
 * where no zone is readable (some vendors lock the node down) the battery temperature stands in,
 * so the figure never goes blank. It does not travel through the engine.
 */
class Thermal(private val ctx: Context) {
    @Volatile private var cpuZone: File? = null

    fun sampleCelsius(): Double? = readCpuZone() ?: readBattery()

    private fun readCpuZone(): Double? {
        val zone = cpuZone ?: discover()?.also { cpuZone = it } ?: return null
        val raw = runCatching { zone.readText().trim().toDouble() }.getOrNull() ?: return null
        return normalise(raw).takeIf { it in 1.0..150.0 }
    }

    private fun discover(): File? {
        val zones = File("/sys/class/thermal").listFiles { f -> f.name.startsWith("thermal_zone") } ?: return null
        return zones.firstOrNull { z ->
            runCatching { File(z, "type").readText().trim().contains("cpu", ignoreCase = true) }.getOrDefault(false)
        }?.let { File(it, "temp") }
    }

    /** Kernel thermal `temp` is conventionally millidegrees, but some vendors report tenths or degrees. */
    private fun normalise(raw: Double): Double = when {
        raw > 1000.0 -> raw / 1000.0
        raw > 200.0 -> raw / 10.0
        else -> raw
    }

    private fun readBattery(): Double? {
        val tenths = runCatching {
            ctx.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
                ?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE) ?: Int.MIN_VALUE
        }.getOrDefault(Int.MIN_VALUE)
        return if (tenths != Int.MIN_VALUE) tenths / 10.0 else null
    }
}
