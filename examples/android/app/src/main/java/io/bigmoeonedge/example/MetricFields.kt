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

        MetricField("loop_overhead_ms", "time between tokens", "everything outside llama_decode: sampling, detokenization, rendering the answer, writing the sinks. It falls outside wall_ms and so outside the reported tok/s, which is why it needs a column — work that moves only this number is paid on every token and shows up nowhere else. On the first token, the gap from the end of prefill to the first decode", Better.LOWER),

        MetricField("drain_ms", "wait on previous batch", "overlap only: eval-thread time waiting for the PREVIOUS layer's reads to finish before reusing their slots — part of compute_ms, named so a large compute residual can be attributed instead of guessed at", Better.LOWER),
        MetricField("adopt_ms", "wait adopting committed reads", "route-ahead only: the load waiting for its own committed speculative reads to complete before staging — part of mgmt_ms", Better.LOWER),
        MetricField("ra_issue_ms", "issuing early reads", "route-ahead only: eval-thread time issuing the committed selection's reads (settle, residency, retain, prefetch) — part of compute_ms", Better.LOWER),
        MetricField("ra_wd_ms", "watchdog control", "route-ahead only: the sampled fresh-gate control's exact GEMV on the eval thread — part of compute_ms", Better.LOWER),

        MetricField("mem_available_mib", "device 'available' (it lies)", "what the kernel claims is free — it counts our own mmap'd weights as reclaimable, so it over-states headroom", Better.NEUTRAL),
        MetricField("mem_free_mib", "device free", "truly free RAM, before any reclaim", Better.NEUTRAL),
        MetricField("swap_free_mib", "swap free", "zram space left before the device is truly out of room", Better.NEUTRAL),
    )

    private val byName = all.associateBy { it.name }
    fun of(name: String): MetricField? = byName[name]
}

/** One configuration key from the CSV preamble, in the words its own source uses. */
data class ConfigField(val name: String, val what: String)

/**
 * What each key of the `# bmoe_metrics v2` preamble means. Same contract as [MetricFields], one
 * level up: those describe what the run measured, these describe what it was measured UNDER. A key
 * the engine adds and this list does not mention still displays — under its raw name, unexplained,
 * which is the right failure and not a reason to hide it.
 *
 * The wording is taken from where each knob is defined (`core/include/bmoe/config.h`) rather than
 * re-invented here, so a reader who follows a setting into the engine finds the same sentence.
 */
object ConfigFields {
    val all: List<ConfigField> = listOf(
        ConfigField("engine", "the engine build that produced these rows. Runs from different builds are not a comparison"),
        ConfigField("model", "the gguf, by filename"),
        ConfigField("arch", "architecture the gguf declares; it selects the streaming recipe"),
        ConfigField("n_layer", "transformer layers"),
        ConfigField("n_expert", "experts per layer the model holds"),
        ConfigField("n_expert_used", "experts routed per token — the model's own width, or an override. Fewer is faster and changes the answer"),
        ConfigField("n_ctx", "context window reserved for the session"),
        ConfigField("n_ubatch", "prefill batch width (0 = default). Decode is one token wide regardless, so this trades prefill throughput for memory"),
        ConfigField("threads", "compute threads"),
        ConfigField("chatml", "the model's own chat template was applied; the key name is historical"),
        ConfigField("moe_stream", "expert streaming. Off is the mmap baseline that streamed runs are measured against"),
        ConfigField("cache_mb", "expert-cache budget the run resolved to, whether given explicitly or chosen by auto-sizing"),
        ConfigField("cache_auto", "the budget was sized from free RAM once at load, then held for the run"),
        ConfigField("cache_floor_mb", "RAM auto-sizing leaves free for the rest of the system. Applies only with cache_auto"),
        ConfigField("cache_ceil_mb", "upper bound on the auto budget; 0 caps only at the full expert-set size"),
        ConfigField("force_cache", "a budget below the engine's floor was permitted. Such a cache thrashes by design and is slower than no cache — reserved for probing where the floor lies"),
        ConfigField("io_threads", "parallel flash read lanes, including the calling thread. 1 is the serial baseline"),
        ConfigField("o_direct", "expert reads bypassed the page cache"),
        ConfigField("overlap", "reads ran during compute instead of blocking before it"),
        ConfigField("io_two_wave", "the first projection's reads were published to the lanes as soon as they were staged, rather than after the whole layer"),
        ConfigField("load_all", "every expert was loaded each token, routing ignored. An A/B baseline"),
        ConfigField("prefetch", "temporal prefetch depth in layers, betting this token routes like the last. 0 = off"),
        ConfigField("route_ahead", "each layer's expert choice was COMMITTED to the prediction made this many layers earlier in the same forward pass, and read that early. Lossy: a fraction of choices differ from the router's own. 0 = off"),
        ConfigField("predict_prefetch", "the next layer's router ran early, and the cache acted on that prediction instead of the previous token's routing"),
        ConfigField("predict_spec_max", "how many predicted, non-resident experts were read ahead. 0 = retention only, which spends no flash and merely protects predicted residents from eviction. Recorded but unused when predict_prefetch is off"),
        ConfigField("predict_log", "prediction-accuracy probe. A diagnostic, not a shipping setting"),
        ConfigField("prefetch_sync", "reads completed synchronously so the byte-identity gates could reach a path a timing race rarely hits. Not for production"),
        ConfigField("dense_weights", "residency policy for the non-expert weights: mmap (baseline), warm (page-cached at load), anon (O_DIRECT into our own buffers, so reclaim goes to zram rather than flash), ahwb (as anon, in dma-buf memory the kernel cannot reclaim; shown as Pinned in Settings)"),
        ConfigField("drop_cold_frac", "dropping threshold as a fraction of the uniform share 1/top-k. A routed expert below it was skipped when not already in RAM. Lossy, and not repeatably so: what was skipped depended on cache contents"),
        ConfigField("drop_renorm", "surviving weights were rescaled so the routing summed to what it did before the drop"),
        ConfigField("drop_prefill", "dropping was applied during prefill as well. Off by default, since a cold cache makes the same threshold discard far more weight there"),
        ConfigField("temp", "sampling temperature. 0 is greedy decoding — reproducible, and what the gates rely on"),
        ConfigField("top_k", "sampling candidates kept by rank. Unrelated to n_expert_used, which is the routing width"),
        ConfigField("top_p", "sampling candidates kept by cumulative probability"),
        ConfigField("seed", "sampling seed. Inert at temperature 0; otherwise the only thing that makes a run repeatable"),
        ConfigField("compute_trace_layers", "per-layer compute tracing. It instruments the graph, so a traced run is a diagnostic rather than a benchmark"),
        ConfigField("spec", "which source drafted for speculative decoding: off, mtp (the model's own trained head) or ngram (repeated text looked up in the prompt and the answer so far). Both verify identically, so this changes what a draft cost, not what was accepted"),
        ConfigField("spec_draft_max", "tokens drafted per verify pass. The pass is one wider than this, and every position in it routes its own experts — which is what speculation trades for confirming several tokens at once"),
        ConfigField("mtp_p_min", "the head stopped drafting below this confidence, making the width adaptive. 0 = always draft the full width. Applies to spec=mtp only"),
        ConfigField("ngram_min_match", "shortest run of repeated tokens the lookup would draft from. Below it the step drafted nothing and cost exactly an unspeculated decode. Applies to spec=ngram only"),
    )

    private val byName = all.associateBy { it.name }
    fun of(name: String): ConfigField? = byName[name]
}
