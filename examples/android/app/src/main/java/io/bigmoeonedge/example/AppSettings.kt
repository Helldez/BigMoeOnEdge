package io.bigmoeonedge.example

import android.content.Context

/**
 * How the dense (non-expert) weights are kept resident — one policy, mirroring the engine's
 * `--dense-weights` flag (core/src/moe/dense_weights.h). Replaces the old warm-dense + dense-O_DIRECT
 * pair of switches, which could express the same policy two ways (or contradict each other).
 */
enum class DenseWeights(val flag: String, val label: String, val blurb: String) {
    MMAP("mmap", "Mmap (baseline)", "Left to the kernel, faulted in as touched. The baseline."),
    WARM("warm", "Warm at load", "Paged in once at load, so the first tokens do not fault. Best when the model fits in RAM."),
    ANON("anon", "Anon (O_DIRECT)", "Read into the app's own memory. Reclaim compresses them instead of dropping them, so a refault costs no flash read. The default."),
    AHWB("ahwb", "Pinned (dma-buf)", "As Anon, but reclaim cannot touch it at all, not even to compress. Pays off over a long conversation."),
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
    // Context the session is opened with: prompt plus reply for the whole conversation. It is also
    // memory — the KV cache is sized for it once at open — so on a model that already fills RAM a
    // shorter context hands the difference back to the expert cache and the dense weights.
    val sessionCtx: Int = SESSION_CTX,
    val oDirect: Boolean = true,        // bypass the page cache
    val overlap: Boolean = true,        // read the next experts while the current layer computes
    val denseWeights: DenseWeights = DenseWeights.ANON, // dense (non-expert) weight residency policy
    val prefetchLayers: Int = 0,        // temporal prefetch depth K (0 = off); needs the cache
    // Predictive prefetch (experimental): run the NEXT layer's router on the current layer's
    // input and speculate/retain on that prediction instead of the previous token's routing.
    // Needs the cache; the engine rejects it combined with the temporal prefetch above, so
    // sessionArgv only emits it when prefetchLayers == 0.
    val predictPrefetch: Boolean = false,
    // How many predicted MISSES per layer it may read ahead (0 = retention only: the prediction
    // spends no flash and only protects predicted residents from eviction). Defaults to 0 because
    // the matched-pair A/B showed read-ahead losing on a saturated flash (docs/expert-prediction.md).
    val predictSpecMax: Int = 0,
    // Route-ahead (experimental, LOSSY): decode routing at layer L is COMMITTED to the prediction
    // made N layers earlier in the same forward pass, and with the cache on the committed experts
    // are read that early — reads that can never be wasted, since they ARE the routing. Changes
    // the output (~20% of slots re-route at N=1; quality held in the first host A/B). Excludes
    // both prefetchers, so sessionArgv only emits it when they are off. 0 = off, the default
    // until the on-device A/B earns it more.
    val routeAhead: Int = 0,
    // Cache-aware expert dropping, as a PERCENTAGE of the uniform share 1/top-k (0 = off, 100 = the
    // share itself). Stored as an Int because the settings are integer rungs; the flag takes a
    // fraction. LOSSY and cache-dependent — it changes the output, and not reproducibly.
    val dropColdPct: Int = 75,
    // Which source drafts for self-speculation: "off", "mtp" or "ngram". Both verify the same way —
    // one wider decode, greedy acceptance — and differ only in what a draft costs.
    //
    // "mtp" uses the model's own head, so it needs a gguf carrying the nextn block, which Qwen3.5/3.6
    // do in their ordinary quantisations — the catalog's Qwen3.6 included, so no special "-MTP-"
    // download. On anything else the engine refuses to open, so the UI states the requirement rather
    // than letting the session fail.
    //
    // "ngram" drafts by looking the recent tokens up in the prompt and in what has been generated:
    // no head, no draft context, no expert read, and it works on every model. It also drafts nothing
    // when it has no confident match, so a step without one costs exactly a plain decode — the
    // property the head does not have.
    //
    // Off by default. The head is a clear win where decode is DRAM-bound (desktop, +15%) and lost on
    // this phone at every draft width; the lookup exists because the measured reason for that loss
    // was the widened verify batch, not the drafting — which is what this toggle now lets an A/B
    // separate.
    val spec: String = SPEC_OFF,
    // Tokens drafted per verify pass, shared by both sources. 3 is the measured optimum for the head
    // on desktop: acceptance falls as the draft widens, and past its horizon the extra drafts are
    // paid for and thrown away. On this phone the device A/B measured 2 better than 3 and both below
    // the baseline, which is why the confidence floor below exists.
    val mtpDraft: Int = 3,
    // Stop drafting when the head's own probability for what it is proposing drops below this
    // percentage (0 = never stop, draft the full width however unsure it is). Makes the width
    // adaptive per step, which on this device pays twice: a draft not made is one fewer pass
    // through the MTP block AND one fewer independently routed position in the verify batch. 0 is
    // the setting the desktop numbers were measured at, so it stays the default until the A/B says.
    val mtpPMinPct: Int = 0,
    val thinking: Boolean = false,      // reasoning; off passes --no-think (enable_thinking=false)
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
            "-c", sessionCtx.toString(),
            // Never reserve a graph wider than the context itself.
            "--ubatch", minOf(SESSION_UBATCH, sessionCtx).toString(),
            // Render the model's OWN chat template, whichever family it belongs to; the flag name
            // is historical (ChatML is only llama.cpp's fallback when a gguf ships no template).
            // Nothing here selects a format, so it is correct for every model in the catalog.
            "--chatml",
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
            // Predictive prefetch shares the cache requirement and excludes the temporal one —
            // two predictors would double-speculate the same future, and the engine refuses the
            // pair. Temporal wins when both are somehow set; the UI keeps them exclusive anyway.
            if (predictPrefetch && cacheOn && prefetchLayers == 0) {
                a += "--predict-prefetch"
                a += listOf("--predict-spec-max", predictSpecMax.toString())
            }
            // Route-ahead excludes both prefetchers (the engine refuses the pairs: they would
            // speculate a future route-ahead has already fixed) and self-speculation (a verify
            // decode is several positions wide, and route-ahead declines to commit on every one of
            // them while still paying for the prediction). It works without the cache too — the
            // routing still commits — but only reads early when the cache is on.
            if (routeAhead > 0 && prefetchLayers == 0 && !predictPrefetch && spec == SPEC_OFF) {
                a += listOf("--route-ahead", routeAhead.toString())
            }
            // Cache-aware dropping needs a live cache to ask about residency — with the cache off
            // every expert reads as a miss and the engine rejects the combination outright, so the
            // same cacheOn condition that guards prefetch guards this. The engine takes a fraction
            // of the uniform share; the setting is stored as a percentage.
            if (dropColdPct > 0 && cacheOn) a += listOf("--drop-cold-experts", (dropColdPct / 100.0).toString())
        }
        // Outside the streaming block on purpose: speculation is a decode-loop change, not a
        // residency policy, so it applies to the mmap baseline too — which is what makes an A/B of
        // the two against each other meaningful.
        if (spec == SPEC_MTP) {
            a += listOf("--mtp", "--draft", mtpDraft.toString())
            if (mtpPMinPct > 0) a += listOf("--mtp-p-min", (mtpPMinPct / 100.0).toString())
        } else if (spec == SPEC_NGRAM) {
            a += listOf("--ngram", "--draft", mtpDraft.toString())
        }
        return a
    }

    /**
     * Identity of the session these settings would open for [modelPath]. Two settings with the
     * same signature can reuse one loaded process (keeping the cache warm); a change means the
     * running session must be torn down and reopened. Per-prompt fields (n_predict, thinking) are
     * excluded: they travel per request and never touch the loaded model.
     *
     * DERIVED FROM THE ARGV, deliberately, rather than listed by hand. The session's identity IS
     * the command line it would open, so a setting that changes the argv changes the signature by
     * construction and one that does not, does not. The hand-written list this replaces had to be
     * kept in step with [sessionArgv] with nothing enforcing it, and forgetting a field there is a
     * silent bug: the setting appears to change while the engine keeps running the old one.
     *
     * The CSV path is excluded (null) because it carries a timestamp: including it would make
     * every session unique and defeat warm reuse entirely.
     */
    fun sessionSignature(modelPath: String): String =
        sessionArgv(cliPath = "", modelPath = modelPath, csvPath = null).joinToString("|")

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
            .putBoolean("predictPrefetch", predictPrefetch)
            .putInt("predictSpecMax", predictSpecMax)
            .putInt("routeAhead", routeAhead)
            .putInt("dropColdPct", dropColdPct)
            .putInt("sessionCtx", sessionCtx)
            .putString("spec", spec).putInt("mtpDraft", mtpDraft).putInt("mtpPMinPct", mtpPMinPct)
            .putBoolean("thinking", thinking)
            .putBoolean("metricsCsv", metricsCsv)
            .apply()
    }

    companion object {
        // Fixed context for a session: sized once at open (no prompt in hand), roomy enough for a
        // long prompt plus the largest practical generation. A request that would overflow it is
        // rejected recoverably by the CLI, leaving the session usable.
        const val SESSION_CTX = 4096

        // Widest graph computed at once. Compute buffers are RESERVED for it, so leaving it at the
        // context width (the engine's default) hands the whole reservation to a model that only
        // ever decodes one token at a time. That reservation is memory the expert cache and the
        // dense weights do not get: measured +18% decode on a model near RAM, and on a >RAM model
        // with a large dense set it is the difference between decoding and swapping (DeepSeek V4
        // spent 13.9 s/token of "compute" that was really page faults, against 1.5 s at this
        // width). Prefill pays instead, and barely: chunking it costs ~7.7x the flash reads but
        // only ~6% of prefill wall time, because prefill is compute-bound.
        const val SESSION_UBATCH = 512

        // Context rungs. 4096 is the default a chat wants; the shorter ones exist for a model that
        // already fills RAM, where the KV cache competes with the weights themselves.
        val CTX_CHOICES = intArrayOf(512, 1024, 2048, 4096, 8192)

        // Tokens to generate per turn, when nothing says otherwise. The service falls back to this
        // for a request that arrives without one, so the default lives here rather than in two
        // places free to disagree. 128 matches the CLI default (issue #71): shorter budgets
        // truncate most answers mid-sentence, which reads as broken rather than slow.
        const val DEFAULT_N_PREDICT = 128

        // The draft sources, as stored. Strings rather than an enum ordinal: a preference that
        // survives an app update must not depend on the order this list happens to be written in.
        const val SPEC_OFF = "off"
        const val SPEC_MTP = "mtp"
        const val SPEC_NGRAM = "ngram"
        val SPEC_CHOICES = arrayOf(SPEC_OFF, SPEC_MTP, SPEC_NGRAM)

        // Draft widths worth offering. The useful range is small and not monotonic: acceptance
        // falls as the draft widens while tokens-per-decode rises, and on desktop the two cross at
        // 3 — 4 measured WORSE than 2. Stopping at 5 keeps the picker honest about that.
        val MTP_DRAFT_CHOICES = intArrayOf(1, 2, 3, 4, 5)

        // Confidence floors worth offering, as percentages. 0 keeps the current behaviour (draft
        // the full width unconditionally); the rest trade speculative reach for wasted drafts.
        val MTP_P_MIN_CHOICES = intArrayOf(0, 40, 60, 80)

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
        // Speculated predicted misses per layer. 0 = retention only (zero flash spent) and the app
        // default — the matched-pair A/B showed 2 losing −21% on a saturated flash; anything above
        // it re-buys the measured full-speculation pathology.
        val PREDICT_SPEC_CHOICES = intArrayOf(0, 1, 2, 4)
        // Route-ahead depth: how many layers early the routing is committed (and read). Each layer
        // of depth widens the I/O window and the routing perturbation together; 1 is the measured
        // sweet spot on the host, 4 the edge where damage first showed.
        val ROUTE_AHEAD_CHOICES = intArrayOf(0, 1, 2, 4)
        // Percent of the uniform share 1/top-k. 100 is the share itself and the useful maximum:
        // above it the threshold could exceed every weight in a routing. The rungs below it are the
        // conservative half of the curve, where the replay already beats a top-k cut on both axes.
        val DROP_COLD_CHOICES = intArrayOf(0, 50, 75, 100)
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
                predictPrefetch = p.getBoolean("predictPrefetch", d.predictPrefetch),
                predictSpecMax = p.getInt("predictSpecMax", d.predictSpecMax),
                routeAhead = p.getInt("routeAhead", d.routeAhead),
                dropColdPct = p.getInt("dropColdPct", d.dropColdPct),
                sessionCtx = p.getInt("sessionCtx", d.sessionCtx),
                spec = run {
                    val saved = p.getString("spec", null)
                    when {
                        saved != null && saved in SPEC_CHOICES -> saved
                        // Migrate the old boolean pref from an install that only had the head.
                        p.getBoolean("mtp", false) -> SPEC_MTP
                        else -> d.spec
                    }
                },
                mtpDraft = p.getInt("mtpDraft", d.mtpDraft),
                mtpPMinPct = p.getInt("mtpPMinPct", d.mtpPMinPct),
                thinking = p.getBoolean("thinking", d.thinking),
                metricsCsv = p.getBoolean("metricsCsv", d.metricsCsv),
            )
        }

        private fun Context.prefs() = getSharedPreferences("bmoe_settings", Context.MODE_PRIVATE)
    }
}
