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
    val thinking: Boolean = false,// reasoning; off passes --no-think (enable_thinking=false)
    val loadAll: Boolean = false,// debug: read ALL experts each token (A/B baseline)
) {
    /**
     * Build the bmoe-cli argument list. The engine renders the model family's own chat template
     * via --chatml; reasoning is turned off with --no-think, which the template honours through
     * its `enable_thinking` kwarg. This is model-agnostic (works for Qwen3 and Gemma alike),
     * unlike the old Qwen-only /no_think prompt suffix. `arch` is kept for future arch-specific
     * handling but is no longer needed for the thinking switch.
     *
     * When [mmap] is set, expert streaming is turned off entirely: the CLI omits --moe-stream and
     * every streaming knob (cache / lanes / O_DIRECT / overlap / load-all), so llama.cpp loads the
     * whole model via mmap through the page cache. This is the baseline the streaming modes are
     * compared against; the prompt/template and compute flags still apply.
     */
    fun toArgv(cliPath: String, modelPath: String, prompt: String, arch: String?): List<String> {
        val a = mutableListOf(
            cliPath,
            "-m", modelPath,
            "-p", prompt,
            "-n", nPredict.toString(),
            "-t", threads.toString(),
            "--chatml", // apply the model family's chat turn (gemma / chatml)
            "--progress",
        )
        if (!thinking) a += "--no-think"
        if (!mmap) {
            a += "--moe-stream"
            a += listOf("--cache-mb", cacheMb.toString())
            a += listOf("--io-threads", ioThreads.toString())
            if (!oDirect) a += "--no-odirect"
            if (overlap) a += "--overlap"
            if (loadAll) a += "--load-all"
        }
        return a
    }

    fun save(ctx: Context) {
        ctx.prefs().edit()
            .putBoolean("mmap", mmap)
            .putInt("cacheMb", cacheMb).putInt("ioThreads", ioThreads).putInt("threads", threads)
            .putInt("nPredict", nPredict).putBoolean("oDirect", oDirect)
            .putBoolean("overlap", overlap)
            .putBoolean("thinking", thinking).putBoolean("loadAll", loadAll)
            .apply()
    }

    companion object {
        // 1000 is deliberately absent: a budget below the ~1500 MiB pathological band
        // thrashes and is slower than no cache — the engine rejects it. Use 0 or >= 2000.
        val CACHE_CHOICES = intArrayOf(0, 2000, 3000, 4000, 5000, 6000)
        val IO_CHOICES = intArrayOf(1, 2, 4, 8)
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
                thinking = p.getBoolean("thinking", d.thinking),
                loadAll = p.getBoolean("loadAll", d.loadAll),
            )
        }

        private fun Context.prefs() = getSharedPreferences("bmoe_settings", Context.MODE_PRIVATE)
    }
}
