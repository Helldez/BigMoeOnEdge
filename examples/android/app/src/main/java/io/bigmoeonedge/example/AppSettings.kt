package io.bigmoeonedge.example

import android.content.Context

/**
 * All user-tunable run options, persisted across launches. These map directly to bmoe-cli
 * flags; the Settings screen edits them and [toArgv] builds the command line.
 */
data class AppSettings(
    val mmap: Boolean = false,   // baseline: no streaming — llama.cpp mmap loads the whole model
    val cacheMb: Int = 4000,     // LRU expert cache budget; 0 or >= 2000 (see CACHE_CHOICES)
    val ioThreads: Int = 4,      // parallel expert-read lanes
    val threads: Int = 4,        // compute threads (-t); 4 is the measured optimum
    val nPredict: Int = 48,
    val oDirect: Boolean = true, // bypass the page cache
    val overlap: Boolean = false,// prefetch experts while the layer computes (experimental)
    val prefetchLayers: Int = 0, // temporal prefetch depth K (0 = off); needs the cache
    val specGate: Boolean = false,// predict next layer's experts via its router; needs the cache
    val thinking: Boolean = false,// reasoning; off passes --no-think (enable_thinking=false)
    val loadAll: Boolean = false,// debug: read ALL experts each token (A/B baseline)
) {
    /**
     * Build the argv that OPENS a persistent bmoe-cli session (`--session`): everything fixed for
     * the model's lifetime — model path, compute threads, context, chat template, and the whole
     * streaming configuration. Per-prompt options (the prompt itself, n_predict, and reasoning)
     * are NOT here; they travel as JSON requests over stdin, one per generation.
     *
     * When [mmap] is set, expert streaming is turned off entirely: the CLI omits --moe-stream and
     * every streaming knob (cache / lanes / O_DIRECT / overlap / load-all), so llama.cpp loads the
     * whole model via mmap through the page cache — the baseline the streaming modes compare to.
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
        if (!mmap) {
            a += "--moe-stream"
            a += if (cacheMb == CACHE_AUTO) listOf("--cache-mb", "auto")
                 else listOf("--cache-mb", cacheMb.toString())
            a += listOf("--io-threads", ioThreads.toString())
            if (!oDirect) a += "--no-odirect"
            if (overlap) a += "--overlap"
            // Auto sizing is a live LRU cache, so it satisfies the prefetch/spec-gate cache requirement.
            val cacheOn = cacheMb == CACHE_AUTO || cacheMb > 0
            if (prefetchLayers > 0 && cacheOn) a += listOf("--prefetch", prefetchLayers.toString())
            if (specGate && cacheOn) a += "--spec-gate"
            if (loadAll) a += "--load-all"
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
        listOf(modelPath, mmap, cacheMb, ioThreads, threads, oDirect, overlap, prefetchLayers, specGate, loadAll)
            .joinToString("|")

    fun save(ctx: Context) {
        ctx.prefs().edit()
            .putBoolean("mmap", mmap)
            .putInt("cacheMb", cacheMb).putInt("ioThreads", ioThreads).putInt("threads", threads)
            .putInt("nPredict", nPredict).putBoolean("oDirect", oDirect)
            .putBoolean("overlap", overlap).putInt("prefetchLayers", prefetchLayers)
            .putBoolean("specGate", specGate)
            .putBoolean("thinking", thinking).putBoolean("loadAll", loadAll)
            .apply()
    }

    companion object {
        // Fixed context for a session: sized once at open (no prompt in hand), roomy enough for a
        // long prompt plus the largest practical generation. A request that would overflow it is
        // rejected recoverably by the CLI, leaving the session usable.
        const val SESSION_CTX = 4096

        // -1 (Auto) sizes the cache to the device's free RAM at runtime (--cache-mb auto). 1000 is
        // deliberately absent: a budget below the ~1500 MiB pathological band thrashes and is slower
        // than no cache — the engine rejects it. Use Auto, 0, or >= 2000.
        const val CACHE_AUTO = -1
        val CACHE_CHOICES = intArrayOf(CACHE_AUTO, 0, 2000, 3000, 4000, 5000, 6000)
        val IO_CHOICES = intArrayOf(1, 2, 4, 8)
        val PREFETCH_CHOICES = intArrayOf(0, 1, 2, 4)
        val THREAD_CHOICES = intArrayOf(2, 4, 6, 8)
        val NPREDICT_CHOICES = intArrayOf(16, 32, 48, 64, 128, 256, 512, 1024, 2048)

        fun load(ctx: Context): AppSettings {
            val p = ctx.prefs()
            val d = AppSettings()
            return AppSettings(
                mmap = p.getBoolean("mmap", d.mmap),
                cacheMb = p.getInt("cacheMb", d.cacheMb),
                ioThreads = p.getInt("ioThreads", d.ioThreads),
                threads = p.getInt("threads", d.threads),
                nPredict = p.getInt("nPredict", d.nPredict),
                oDirect = p.getBoolean("oDirect", d.oDirect),
                overlap = p.getBoolean("overlap", d.overlap),
                prefetchLayers = p.getInt("prefetchLayers", d.prefetchLayers),
                specGate = p.getBoolean("specGate", d.specGate),
                thinking = p.getBoolean("thinking", d.thinking),
                loadAll = p.getBoolean("loadAll", d.loadAll),
            )
        }

        private fun Context.prefs() = getSharedPreferences("bmoe_settings", Context.MODE_PRIVATE)
    }
}
