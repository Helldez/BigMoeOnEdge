package io.bigmoeonedge.example

/**
 * What each metrics-CSV column means, in one place. The engine writes the columns
 * (core/src/metrics/csv_metrics_sink.cpp); this is the reader's side of that contract, so a column
 * added there and not described here still plots — it just shows up unexplained, which is the right
 * failure. Keep the names in exact sync with the header the engine writes.
 */
enum class Better { LOWER, HIGHER, NEUTRAL }

data class MetricField(
    val name: String,
    val short: String,   // what it is, in a phrase
    val measures: String, // what the number actually counts
    val better: Better,   // and which direction is good — NEUTRAL when "it depends" is the honest answer
)

object MetricFields {
    val all: List<MetricField> = listOf(
        MetricField("turn", "which generation", "0-based turn in the session; one file spans them all", Better.NEUTRAL),
        MetricField("step", "token index", "this token's number within the turn, from 1", Better.NEUTRAL),
        MetricField("steps", "tokens requested", "the n_predict target for the turn", Better.NEUTRAL),

        MetricField("wall_ms", "token time", "the whole time this token took — the one number measured directly; tok/s = 1000/wall_ms", Better.LOWER),
        MetricField("compute_ms", "compute (a residual!)", "with streaming on, what is left after io/stall and mgmt are subtracted — NOT measured, so it also absorbs faults and scheduler stalls, and is clamped at 0. With streaming off it is just wall_ms", Better.LOWER),
        MetricField("io_ms", "flash read time", "time reading experts from flash. In serial it is part of wall_ms; under overlap it is per-lane busy time summed, so it can exceed wall_ms", Better.LOWER),
        MetricField("stall_ms", "wait on flash", "overlap only: the wall time compute actually sat idle waiting for a read (already divided per thread)", Better.LOWER),
        MetricField("mgmt_ms", "cache bookkeeping", "time committing, evicting and reordering the LRU cache this token — plus the periodic dense-residency probe, which the engine folds in here", Better.LOWER),

        MetricField("read_bytes", "flash read", "bytes of experts pulled from flash THIS token (not cumulative)", Better.LOWER),
        MetricField("cache_hit_pct", "hit rate (cumulative!)", "share of expert lookups served from cache — averaged from the start of the session and INCLUDING prefill (which dominates early), so it moves slowly and is not a per-token value", Better.HIGHER),

        MetricField("majflt", "hard faults", "pages the kernel reclaimed and the decode had to re-read from flash mid-compute this token", Better.LOWER),
        MetricField("majflt_mib", "faulted MiB", "the same faults as memory: majflt × the kernel page size (4 KiB, or 16 KiB on large-page devices — resolved at runtime, not assumed). Compare directly to read_bytes — reads we chose vs reads the kernel forced", Better.LOWER),
        MetricField("cpu_ms", "CPU used", "CPU time across all threads. Compare to wall_ms x threads: near 100% is compute-bound, far below means throttled or stalled cores", Better.NEUTRAL),

        MetricField("cache_budget_mib", "cache budget", "the expert-cache size this run used. Fixed for the run (an explicit --cache-mb, or what auto sized to at load). Bigger buys hits but costs RAM", Better.NEUTRAL),
        MetricField("dense_resident_frac", "dense weights still in RAM", "estimated fraction of the DENSE (non-expert) weights still resident — a 256-page mincore SAMPLE, not the exact fraction, refreshed every few tokens so most rows carry the last sample. Under 'anon' it samples our own buffers (is zram holding them?); under mmap/warm the mmap (is the kernel dropping the model?). Read with majflt. -1 only before the first sample, with streaming off, or when /proc is unreadable", Better.HIGHER),

        MetricField("rss_anon_mib", "anon resident", "resident anonymous memory: the expert cache and the KV/activation buffers — and, under the default 'anon' dense policy, the dense weights too. Falling while cache_budget holds = the kernel taking it", Better.NEUTRAL),
        MetricField("rss_file_mib", "file resident", "resident file-backed memory: the mmap'd weights, dropped (not swapped) under pressure so it never shows in swap. But under the default 'anon' dense policy the dense weights are anonymous, not file-backed — then this is only what is left mmap'd, and rss_anon carries the model", Better.NEUTRAL),
        MetricField("swap_mib", "our memory in zram", "anonymous memory already compressed into swap — cache we have effectively lost", Better.LOWER),
        MetricField("rss_mib", "total resident", "everything resident (VmRSS)", Better.NEUTRAL),

        MetricField("mem_available_mib", "device 'available' (it lies)", "what the kernel claims is free — it counts our own mmap'd weights as reclaimable, so it over-states headroom", Better.NEUTRAL),
        MetricField("mem_free_mib", "device free", "truly free RAM, before any reclaim", Better.NEUTRAL),
        MetricField("swap_free_mib", "swap free", "zram space left before the device is truly out of room", Better.NEUTRAL),
    )

    private val byName = all.associateBy { it.name }
    fun of(name: String): MetricField? = byName[name]
}
