package io.bigmoeonedge.example

import android.content.Context

/**
 * How the dense (non-expert) weights are kept resident — one policy, mirroring the engine's
 * `--dense-weights` flag (core/src/moe/dense_weights.h). Replaces the old warm-dense + dense-O_DIRECT
 * pair of switches, which could express the same policy two ways (or contradict each other).
 */
enum class DenseWeights(val flag: String, val label: String, val blurb: String) {
    MMAP("mmap", "Mmap (baseline)", "Leave the dense weights mmap'd; the kernel faults them in. Slow first tokens on a >RAM model — the A/B baseline."),
    WARM("warm", "Warm at load", "Page-cache the dense weights once at load, so the first tokens don't fault them in a page at a time. Best when the model fits in RAM."),
    ANON("anon", "Anon (O_DIRECT)", "Read the dense weights via O_DIRECT into our own buffers so a reclaim swaps to zram (fast) instead of a slow flash refault. Costs a private copy. The default — it wins on >RAM models."),
}

/**
 * All user-tunable run options, persisted across launches. These map directly to bmoe-cli
 * flags; the Settings screen edits them and [toArgv] builds the command line.
 */
data class AppSettings(
    // Predictable, device-agnostic defaults: a fixed 2000 MiB expert cache (reproducible across
    // runs, unlike Auto which sizes to whatever RAM happens to be free — issue #71), streaming
    // with overlap, 4 compute + 4 read lanes, model's own top-k. No device- or
    // benchmark-specific tuning — the knobs below let the user tune for their own hardware.
    val mmap: Boolean = false,          // baseline: no streaming — llama.cpp mmap loads the whole model
    val cacheMb: Int = 2000,            // LRU expert cache budget; Auto / 0 / 500..6000 (see CACHE_CHOICES)
    val cacheCeilMb: Int = 3000,        // with cacheMb=Auto: upper bound on the auto budget (0 = no cap)
    val ioThreads: Int = 4,             // parallel expert-read lanes
    val threads: Int = 4,               // compute threads (-t)
    val nExpertUsed: Int = 0,           // top-k override (0 = model default); lower = faster, changes output
    val nPredict: Int = DEFAULT_N_PREDICT,
    val oDirect: Boolean = true,        // bypass the page cache
    val overlap: Boolean = true,        // read the next experts while the current layer computes
    val denseWeights: DenseWeights = DenseWeights.ANON, // dense (non-expert) weight residency policy
    val prefetchLayers: Int = 0,        // temporal prefetch depth K (0 = off); needs the cache
    // Reasoning. Per-prompt, so it rides the generate request's "think" field rather than argv.
    // Whether "off" is obeyed depends on the model; the engine reports which mechanism applies as
    // think_ctl in BMOE_READY (UiState.thinkCtl) and Settings says so. See issue #82.
    val thinking: Boolean = false,
    val metricsCsv: Boolean = true,     // write the engine's per-token CSV for this session (--csv)
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
     *
     * @param csvPath where the engine should write its per-token metrics CSV, or null to not ask
     *   for one. One file per SESSION, not per turn: the engine appends every turn's rows to it and
     *   marks each with a `turn` column, which is the only way to read the two-turn shape this
     *   engine is judged by (a fast turn, an idle, then the turn that pays for it).
     */
    fun sessionArgv(cliPath: String, modelPath: String, csvPath: String? = null): List<String> {
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
        // Outside the mmap gate too: the fault and memory columns are measured whether or not the
        // streamer is on, and the mmap baseline is exactly what they are compared against.
        if (metricsCsv && csvPath != null) a += listOf("--csv", csvPath)
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
            // Dense (non-expert) weight policy — one canonical flag (mmap | warm | anon).
            a += listOf("--dense-weights", denseWeights.flag)
            // Auto sizing is a live LRU cache, so it satisfies the prefetch cache requirement.
            val cacheOn = cacheMb == CACHE_AUTO || cacheMb > 0
            if (prefetchLayers > 0 && cacheOn) a += listOf("--prefetch", prefetchLayers.toString())
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
        listOf(modelPath, mmap, cacheMb, cacheCeilMb, ioThreads, threads, nExpertUsed, oDirect,
               overlap, denseWeights, prefetchLayers)
            .joinToString("|")

    fun save(ctx: Context) {
        ctx.prefs().edit()
            .putBoolean("mmap", mmap)
            .putInt("cacheMb", cacheMb).putInt("cacheCeilMb", cacheCeilMb)
            .putInt("ioThreads", ioThreads).putInt("threads", threads)
            .putInt("nExpertUsed", nExpertUsed)
            .putInt("nPredict", nPredict).putBoolean("oDirect", oDirect)
            .putBoolean("overlap", overlap)
            .putString("denseWeights", denseWeights.name)
            .putInt("prefetchLayers", prefetchLayers)
            .putBoolean("thinking", thinking)
            .putBoolean("metricsCsv", metricsCsv)
            .apply()
    }

    companion object {
        // Fixed context for a session: sized once at open (no prompt in hand), roomy enough for a
        // long prompt plus the largest practical generation. A request that would overflow it is
        // rejected recoverably by the CLI, leaving the session usable.
        const val SESSION_CTX = 4096

        // Tokens to generate per turn, when nothing says otherwise. The service falls back to this
        // for a request that arrives without one, so the default lives here rather than in two
        // places free to disagree. 128 matches the CLI default (issue #71): shorter budgets
        // truncate most answers mid-sentence, which reads as broken rather than slow.
        const val DEFAULT_N_PREDICT = 128

        /**
         * A fresh CSV path for a session about to open, under the app's own external files dir —
         * no permission needed to write, and `adb pull`-able without root:
         *
         *     /sdcard/Android/data/<pkg>/files/metrics/bmoe-<yyyyMMdd-HHmmss>.csv
         *
         * Timestamped rather than fixed, because a run you cannot tell apart from the previous one
         * is not evidence. Returns null if the volume is unavailable, which just means no CSV.
         */
        fun newMetricsCsvPath(ctx: Context): String? {
            val dir = java.io.File(ctx.getExternalFilesDir(null) ?: return null, "metrics")
            if (!dir.isDirectory && !dir.mkdirs()) return null
            val ts = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US).format(java.util.Date())
            return java.io.File(dir, "bmoe-$ts.csv").absolutePath
        }

        /** The most recent metrics CSV, or null if none was written yet. */
        fun latestMetricsCsv(ctx: Context): java.io.File? {
            val root = ctx.getExternalFilesDir(null) ?: return null
            return java.io.File(root, "metrics")
                .listFiles { f -> f.name.endsWith(".csv") }
                ?.maxByOrNull { it.lastModified() }
        }

        // -1 (Auto) sizes the cache to the device's free RAM once at load (--cache-mb auto).
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
                ioThreads = p.getInt("ioThreads", d.ioThreads),
                threads = p.getInt("threads", d.threads),
                nExpertUsed = p.getInt("nExpertUsed", d.nExpertUsed),
                nPredict = p.getInt("nPredict", d.nPredict),
                oDirect = p.getBoolean("oDirect", d.oDirect),
                overlap = p.getBoolean("overlap", d.overlap),
                denseWeights = run {
                    val saved = p.getString("denseWeights", null)
                    when {
                        saved != null -> runCatching { DenseWeights.valueOf(saved) }.getOrDefault(d.denseWeights)
                        // Migrate the old two-boolean prefs from a pre-harmonization install.
                        p.getBoolean("denseOdirect", false) -> DenseWeights.ANON
                        !p.getBoolean("warmDense", true) -> DenseWeights.MMAP
                        // No prior choice: the field default is the one source of truth.
                        else -> d.denseWeights
                    }
                },
                prefetchLayers = p.getInt("prefetchLayers", d.prefetchLayers),
                thinking = p.getBoolean("thinking", d.thinking),
                metricsCsv = p.getBoolean("metricsCsv", d.metricsCsv),
            )
        }

        private fun Context.prefs() = getSharedPreferences("bmoe_settings", Context.MODE_PRIVATE)
    }
}
