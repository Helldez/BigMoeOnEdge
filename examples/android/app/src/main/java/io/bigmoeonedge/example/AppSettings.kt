package io.bigmoeonedge.example

import android.content.Context

/**
 * All user-tunable run options, persisted across launches. These map directly to bmoe-cli
 * flags; the Settings screen edits them and [toArgv] builds the command line.
 */
data class AppSettings(
    // Conservative, device-agnostic defaults: an auto-sized expert cache capped at 3000 MiB,
    // streaming with overlap, 4 compute + 4 read lanes, model's own top-k. No device- or
    // benchmark-specific tuning — the knobs below let the user tune for their own hardware.
    val mmap: Boolean = false,          // baseline: no streaming — llama.cpp mmap loads the whole model
    val cacheMb: Int = CACHE_AUTO,      // LRU expert cache budget; Auto / 0 / 500..6000 (see CACHE_CHOICES)
    val cacheCeilMb: Int = 3000,        // with cacheMb=Auto: upper bound on the auto budget (0 = no cap)
    val cacheDynamic: Boolean = false,  // treat the budget as a ceiling and let the engine find the
                                        // largest cache the device concedes (--cache-dynamic). Off
                                        // until it is measured on device, like every other claim here.
    val ioThreads: Int = 4,             // parallel expert-read lanes
    val threads: Int = 4,               // compute threads (-t)
    val nExpertUsed: Int = 0,           // top-k override (0 = model default); lower = faster, changes output
    val nPredict: Int = 48,
    val oDirect: Boolean = true,        // bypass the page cache
    val overlap: Boolean = true,        // read the next experts while the current layer computes
    val warmDense: Boolean = true,      // page-cache the non-expert weights at load (kills >RAM slow-start)
    val prefetchLayers: Int = 0,        // temporal prefetch depth K (0 = off); needs the cache
    val thinking: Boolean = false,      // reasoning; off passes --no-think (enable_thinking=false)
) {
    /**
     * Build the argv that OPENS a persistent bmoe-cli session (`--session`): everything fixed for
     * the model's lifetime — model path, compute threads, context, chat template, and the whole
     * streaming configuration. Per-prompt options (the prompt itself, n_predict, and reasoning)
     * are NOT here; they travel as JSON requests over stdin, one per generation.
     *
     * When [mmap] is set, expert streaming is turned off entirely: the CLI omits --moe-stream and
     * every streaming knob (cache / lanes / O_DIRECT / overlap), so llama.cpp loads the whole model
     * via mmap through the page cache — the baseline the streaming modes compare to.
     */
    fun sessionArgv(cliPath: String, modelPath: String): List<String> {
        val a = mutableListOf(
            cliPath,
            "-m", modelPath,
            "-t", threads.toString(),
            "-c", SESSION_CTX.toString(),
            "--chatml", // apply the model family's chat turn (gemma / chatml)
            "--session",
        )
        // Active-expert (top-k) override is a load-time kv_override, valid with or without
        // streaming — so it lives outside the mmap gate below.
        if (nExpertUsed > 0) a += listOf("--n-expert-used", nExpertUsed.toString())
        if (!mmap) {
            a += "--moe-stream"
            if (cacheMb == CACHE_AUTO) {
                a += listOf("--cache-mb", "auto")
                if (cacheCeilMb > 0) a += listOf("--cache-ceil-mb", cacheCeilMb.toString())
            } else {
                a += listOf("--cache-mb", cacheMb.toString())
                // The engine refuses a budget under its floor unless told the caller means it; the
                // small rungs exist precisely to probe that floor, so send the override with them.
                if (cacheNeedsForce(cacheMb)) a += "--force-cache"
            }
            a += listOf("--io-threads", ioThreads.toString())
            if (!oDirect) a += "--no-odirect"
            if (overlap) a += "--overlap"
            // Warm-up is on by default in the engine; only surface the opt-out flag.
            if (!warmDense) a += "--no-warm-dense"
            // Auto sizing is a live LRU cache, so it satisfies the prefetch cache requirement.
            val cacheOn = cacheMb == CACHE_AUTO || cacheMb > 0
            if (prefetchLayers > 0 && cacheOn) a += listOf("--prefetch", prefetchLayers.toString())
            // Same requirement: there is no budget to size at runtime with the cache off.
            if (cacheDynamic && cacheOn) a += "--cache-dynamic"
        }
        return a
    }

    /**
     * Identity of the session these settings would open for [modelPath]. Two settings with the
     * same signature can reuse one loaded process (keeping the cache warm); a change means the
     * running session must be torn down and reopened. Per-prompt fields (n_predict, thinking) are
     * excluded — they vary per request without touching the loaded model.
     */
    fun sessionSignature(modelPath: String): String =
        listOf(modelPath, mmap, cacheMb, cacheCeilMb, cacheDynamic, ioThreads, threads, nExpertUsed, oDirect,
               overlap, warmDense, prefetchLayers)
            .joinToString("|")

    fun save(ctx: Context) {
        ctx.prefs().edit()
            .putBoolean("mmap", mmap)
            .putInt("cacheMb", cacheMb).putInt("cacheCeilMb", cacheCeilMb)
            .putBoolean("cacheDynamic", cacheDynamic)
            .putInt("ioThreads", ioThreads).putInt("threads", threads)
            .putInt("nExpertUsed", nExpertUsed)
            .putInt("nPredict", nPredict).putBoolean("oDirect", oDirect)
            .putBoolean("overlap", overlap).putBoolean("warmDense", warmDense)
            .putInt("prefetchLayers", prefetchLayers)
            .putBoolean("thinking", thinking)
            .apply()
    }

    companion object {
        // Fixed context for a session: sized once at open (no prompt in hand), roomy enough for a
        // long prompt plus the largest practical generation. A request that would overflow it is
        // rejected recoverably by the CLI, leaving the session usable.
        const val SESSION_CTX = 4096

        // -1 (Auto) sizes the cache to the device's free RAM at runtime (--cache-mb auto).
        //
        // 500 and 1000 are below the engine's cache_min_mb floor, so picking them makes [sessionArgv]
        // add --force-cache. The floor exists because a cache smaller than one token's routed working
        // set can only thrash — but that verdict was measured on models whose cache pays for itself,
        // and it is exactly what is in question on a >RAM model: gpt-oss-120b at top-2 routes ~886 MB
        // per token and returns an 8-13% hit rate from a 2000-3000 MiB budget, so its cache may
        // already be below the floor's intent while sitting well above its number. These rungs are
        // here to measure where the cache stops earning the memory pressure it creates.
        // See docs/android-memory.md.
        const val CACHE_AUTO = -1
        val CACHE_CHOICES = intArrayOf(CACHE_AUTO, 0, 500, 1000, 2000, 3000, 4000, 5000, 6000)

        /** True for a fixed budget the engine would reject without --force-cache. */
        fun cacheNeedsForce(mb: Int) = mb in 1 until CACHE_MIN_MB
        const val CACHE_MIN_MB = 1500 // mirrors MoeStreamConfig::cache_min_mb
        // Upper bound for the Auto budget (0 = no cap). A cap keeps Auto from over-growing into
        // memory pressure on devices where free RAM is tight.
        val CACHE_CEIL_CHOICES = intArrayOf(0, 2000, 3000, 4000, 5000, 6000)
        val IO_CHOICES = intArrayOf(1, 2, 4, 8)
        // 0 = model default (top-k as trained). 6/4/3/2 trade output quality for tok/s (fewer routed experts).
        val N_EXPERT_CHOICES = intArrayOf(0, 6, 4, 3, 2)
        val PREFETCH_CHOICES = intArrayOf(0, 1, 2, 4)
        val THREAD_CHOICES = intArrayOf(2, 4, 6, 8)
        val NPREDICT_CHOICES = intArrayOf(16, 32, 48, 64, 128, 256, 512, 1024, 2048)

        fun load(ctx: Context): AppSettings {
            val p = ctx.prefs()
            val d = AppSettings()
            return AppSettings(
                mmap = p.getBoolean("mmap", d.mmap),
                cacheMb = p.getInt("cacheMb", d.cacheMb),
                cacheCeilMb = p.getInt("cacheCeilMb", d.cacheCeilMb),
                cacheDynamic = p.getBoolean("cacheDynamic", d.cacheDynamic),
                ioThreads = p.getInt("ioThreads", d.ioThreads),
                threads = p.getInt("threads", d.threads),
                nExpertUsed = p.getInt("nExpertUsed", d.nExpertUsed),
                nPredict = p.getInt("nPredict", d.nPredict),
                oDirect = p.getBoolean("oDirect", d.oDirect),
                overlap = p.getBoolean("overlap", d.overlap),
                warmDense = p.getBoolean("warmDense", d.warmDense),
                prefetchLayers = p.getInt("prefetchLayers", d.prefetchLayers),
                thinking = p.getBoolean("thinking", d.thinking),
            )
        }

        private fun Context.prefs() = getSharedPreferences("bmoe_settings", Context.MODE_PRIVATE)
    }
}
