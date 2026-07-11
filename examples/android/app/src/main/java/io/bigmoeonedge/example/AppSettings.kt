package io.bigmoeonedge.example

import android.content.Context

/**
 * All user-tunable run options, persisted across launches. These map directly to bmoe-cli
 * flags; the Settings screen edits them and [toArgv] builds the command line.
 */
data class AppSettings(
    val cacheMb: Int = 4000,     // LRU expert cache budget; 0 or >= 2000 (see CACHE_CHOICES)
    val ioThreads: Int = 4,      // parallel expert-read lanes
    val threads: Int = 4,        // compute threads (-t); 4 is the measured optimum
    val nPredict: Int = 48,
    val oDirect: Boolean = true, // bypass the page cache
    val thinking: Boolean = false,// Qwen3 reasoning; off appends the /no_think soft switch
    val loadAll: Boolean = false,// debug: read ALL experts each token (A/B baseline)
) {
    /** Build the bmoe-cli argument list. Thinking is toggled with Qwen3's /no_think switch. */
    fun toArgv(cliPath: String, modelPath: String, prompt: String): List<String> {
        val effectivePrompt = if (thinking) prompt else "$prompt /no_think"
        val a = mutableListOf(
            cliPath,
            "-m", modelPath,
            "-p", effectivePrompt,
            "-n", nPredict.toString(),
            "-t", threads.toString(),
            "--moe-stream",
            "--cache-mb", cacheMb.toString(),
            "--io-threads", ioThreads.toString(),
            "--progress",
        )
        if (!oDirect) a += "--no-odirect"
        if (loadAll) a += "--load-all"
        return a
    }

    fun save(ctx: Context) {
        ctx.prefs().edit()
            .putInt("cacheMb", cacheMb).putInt("ioThreads", ioThreads).putInt("threads", threads)
            .putInt("nPredict", nPredict).putBoolean("oDirect", oDirect)
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
                cacheMb = p.getInt("cacheMb", d.cacheMb),
                ioThreads = p.getInt("ioThreads", d.ioThreads),
                threads = p.getInt("threads", d.threads),
                nPredict = p.getInt("nPredict", d.nPredict),
                oDirect = p.getBoolean("oDirect", d.oDirect),
                thinking = p.getBoolean("thinking", d.thinking),
                loadAll = p.getBoolean("loadAll", d.loadAll),
            )
        }

        private fun Context.prefs() = getSharedPreferences("bmoe_settings", Context.MODE_PRIVATE)
    }
}
