package io.bigmoeonedge.app

import io.bigmoeonedge.app.data.CpuZones
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * The CPU temperature comes from a sensor, not from a threshold published as a zone (#311): a fake
 * `/sys/class/thermal` with a constant trip zone listed first and two moving sensors.
 */
class CpuZonesTest {
    @get:Rule val tmp = TemporaryFolder()

    private fun zone(root: File, n: Int, type: String, milli: Int, vararg trips: Int): File {
        val z = File(root, "thermal_zone$n").apply { mkdirs() }
        File(z, "type").writeText("$type\n")
        File(z, "temp").writeText("$milli\n")
        trips.forEachIndexed { i, t -> File(z, "trip_point_${i}_temp").writeText("$t\n") }
        return z
    }

    private fun set(z: File, milli: Int) = File(z, "temp").writeText("$milli\n")

    @Test
    fun aZoneReadingItsOwnTripPointIsNotTheTemperature() {
        val root = tmp.newFolder("thermal")
        zone(root, 64, "cpu-hw-trip-0", 95000, 95000)
        zone(root, 3, "cpu-0-2-1", 47000, 110000)
        zone(root, 7, "cpu-0-4-1", 55000, 110000)
        assertEquals(55.0, CpuZones(root).sample()!!, 0.0)
    }

    @Test
    fun aZoneThatNeverMovesLosesToSensorsThatDo() {
        val root = tmp.newFolder("thermal")
        zone(root, 0, "cpu-trip", 95000) // no trip point published: only its stillness gives it away
        val a = zone(root, 1, "cpu-0-0", 47000)
        val b = zone(root, 2, "cpu-0-1", 50000)
        val zones = CpuZones(root)
        assertEquals(50.0, zones.sample()!!, 0.0) // nothing has moved yet: the median, not 95
        set(a, 58000)
        set(b, 61000)
        assertEquals(61.0, zones.sample()!!, 0.0)
    }

    @Test
    fun theChoiceDoesNotDependOnDirectoryOrder() {
        val root = tmp.newFolder("thermal")
        zone(root, 10, "cpu-a", 52000)
        zone(root, 2, "cpu-b", 52000)
        zone(root, 1, "gpu", 80000)
        assertEquals(52.0, CpuZones(root).sample()!!, 0.0)
    }

    @Test
    fun noReadableCpuZoneLeavesItToTheBattery() {
        val root = tmp.newFolder("thermal")
        zone(root, 0, "battery", 30000)
        assertNull(CpuZones(root).sample())
        assertNull(CpuZones(File(root, "missing")).sample())
    }
}
