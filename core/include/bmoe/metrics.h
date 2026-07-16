// Per-token and end-of-run metrics.
//
// The engine reports one TokenMetrics per generated token and a RunSummary at the end.
// How they are surfaced is a policy choice: the CLI turns TokenMetrics into machine
// telemetry lines (docs/telemetry.md) or an inline stream, and a sink can persist them
// as CSV for benchmarking. The engine itself makes no formatting decisions.
#pragma once

#include <cstdint>
#include <string>

namespace bmoe {

struct TokenMetrics {
    int step = 0;               // 1-based index of this token
    int steps = 0;              // n_predict target
    double wall_ms = 0.0;       // total wall time for this token
    double io_ms = 0.0;         // flash read time this token (serial: subset of wall; overlap: lane-busy sum)
    double mgmt_ms = 0.0;       // cache-management time (vm commit + evict + LRU bookkeeping) this token
    double compute_ms = 0.0;    // residual: serial wall - io - mgmt; overlap wall - stall - mgmt
    double stall_ms = 0.0;      // overlap only: wall time the FFN kernel blocked on flash (0 when serial)
    uint64_t read_bytes = 0;    // expert bytes pulled from flash this token
    double cache_hit_pct = 0.0; // cumulative cache hit rate (-1 if no cache)
    // Compute-decomposition counters, measured around llama_decode (0 if the platform can't report
    // them). They tell WHY a token's compute residual is large: major faults = dense weight re-read
    // from flash inside the decode; cpu_ms vs wall_ms×threads = how CPU-bound the decode really was
    // (low occupancy ⇒ throttled/preempted, not heavy math). See docs/telemetry.md.
    uint64_t majflt = 0; // major page faults during this decode (backing-store reads)
    double cpu_ms = 0.0; // CPU time summed across all threads during this decode
    // Fraction of the expert cache's own pages the kernel still had in RAM at the last sample, or -1
    // when unmeasured (sampler throttled, streaming off, platform can't report). Under 1 the device
    // is reclaiming the cache mid-run — the pressure --cache-dynamic sizes against. See
    // docs/pressure.md.
    double resident_frac = -1.0;
    // Fraction of the DENSE weights (the mmap'd model) still in RAM, or -1 when unmeasured. The other
    // half of memory: resident_frac watches the anon cache, this the file-backed weights. Dense
    // falling while resident_frac holds says the faults are the model, and a smaller cache cannot fix
    // that. See docs/pressure.md.
    double dense_resident_frac = -1.0;
    // What those faults actually moved, in MiB (majflt × page size). The same fact as `majflt`, in
    // the unit the rest of this struct is in: 47447 faults is unreadable, 194 MiB re-faulted in one
    // token is immediately comparable to `read_bytes` — the reads we chose against the reads the
    // kernel forced on us. 0 when faults are unmeasured.
    double majflt_mib = 0.0;

    // ── where memory is, per token (0 when the platform cannot report) ──
    // The split is the point: the expert cache is anonymous, the model's weights are file-backed,
    // and they are reclaimed differently — anon is compressed into zram, file pages are just
    // dropped. rss_anon_mib falling while the budget stays put IS the kernel taking the cache.
    double rss_mib = 0.0;
    double rss_anon_mib = 0.0;
    double rss_file_mib = 0.0;
    double swap_mib = 0.0;
    // What the device claims about itself, recorded next to what we measured ourselves — the gap
    // between mem_available_mib and our own residency is the reason this engine trusts neither.
    double mem_available_mib = 0.0;
    double mem_free_mib = 0.0;
    double swap_free_mib = 0.0;

    // The cache budget as of this token. In the summary it is only the last value; per token it is
    // the governor's trajectory — when it cut, how far, whether it grew back. Without it a run that
    // "did not work" cannot be told apart from one that never acted.
    double cache_budget_mib = 0.0;
    int turn = 0; // session turn this token belongs to (0 for a one-shot run)

    // Governor v2 (--cache-gov2) trajectory: state (0 = on/LRU, 1 = off/slots) and the war it last
    // attributed the pressure to (0 none, 1 cache, 2 dense, 3 anon, 4 foreign). Both 0 otherwise.
    // A demotion shows as gov_state flipping to 1 and cache_budget_mib dropping to 0.
    int gov_state = 0;
    int gov_war = 0;

    std::string piece; // text of just this token (delta, for inline streaming)
    std::string text;  // full generated text so far (for UI streaming)
};

struct RunSummary {
    int n_generated = 0;
    double gen_seconds = 0.0;
    double s_per_token = 0.0;
    double tokens_per_second = 0.0;

    // Startup phase (additive telemetry): model load + streaming setup, and prefill.
    // TTFT ~= load_seconds + prefill_seconds; prefill tok/s = n_prompt / prefill_seconds.
    // In a multi-turn chat n_prompt is the tokens actually prefilled THIS turn (the suffix
    // after the reused KV prefix), and n_past is the total context length after the turn.
    int n_prompt = 0;
    int n_past = 0;
    double load_seconds = 0.0;
    double prefill_seconds = 0.0;

    // MoE streaming totals (zero when streaming is off)
    double moe_read_mib = 0.0;
    double moe_io_seconds = 0.0;
    double moe_compute_s_per_token = 0.0;
    double moe_io_s_per_token = 0.0;
    double moe_mgmt_s_per_token = 0.0;  // cache-management time per token (commit + evict + LRU)
    double moe_stall_s_per_token = 0.0; // overlap only: per-token wall the kernel waited on flash
    double cache_hit_pct = -1.0;        // -1 when no cache

    // Compute decomposition, generation phase only (measured whether or not streaming is on, since
    // dense-weight faults appear in the mmap baseline too). majflt_per_token ≫ 0 flags a residency
    // stall hiding in "compute"; cpu_util = cpu_s/token ÷ (s/token × threads) near 1 is compute-bound,
    // well below 1 is a throttled/preempted core. 0 when the platform can't measure them.
    double majflt_per_token = 0.0;
    double cpu_s_per_token = 0.0;
    double cache_resident_mib = 0.0;
    double cache_budget_mib = 0.0; // current cache budget (moves under --cache-mb auto/--cache-dynamic)
    long long cache_resizes = 0;   // runtime budget changes (0 unless auto/dynamic/explicit resize)

    // What one token actually demands of the cache, measured: the distinct expert bytes routed per
    // token. A cache below this can hold nothing between tokens; a cache far above it is buying
    // hits from inter-token routing correlation only. Reading it against cache_budget_mib is how a
    // budget stops being a guess. 0 when streaming is off or nothing was routed.
    double token_demand_mib = 0.0;
    // The widest layer's routed bytes: the mechanical floor the governor may never cut below.
    double layer_demand_mib = 0.0;
    // Times --cache-dynamic cut the budget because the device was reclaiming the cache (0 when off).
    long long cache_cuts = 0;

    // Temporal prefetch (zero when --prefetch is off): speculative bytes read during generation,
    // experts successfully prefetched, and how many of those a later routing actually used.
    double moe_spec_read_mib = 0.0;
    long long moe_spec_experts = 0;
    long long moe_spec_useful = 0;
};

// What this run IS: the model and the configuration every row below it was produced under.
//
// Rows without it are not evidence. Two CSVs put side by side answer "which is faster" only if
// something says what differed between them — and by the time a file is read, the argv that made
// it is long gone. The engine states it once, in the file, next to the numbers it explains.
struct RunInfo {
    std::string model; // file name, not the full path: the path is the reader's machine, not the run's
    std::string arch;
    int n_layer = 0;
    int n_expert = 0;
    int n_expert_used = 0; // effective top-k, after any override
    int n_threads = 0;
    int n_ctx = 0;

    // The streaming configuration, as resolved (not as typed): cache_mb is what the engine settled
    // on, which under auto-sizing is a number no flag mentioned.
    bool moe_stream = false;
    int cache_mb = 0;
    bool cache_auto = false;
    int cache_ceil_mb = 0;
    bool cache_dynamic = false;
    bool cache_gov2 = false;
    bool force_cache = false;
    int io_threads = 0;
    bool o_direct = false;
    bool overlap = false;
    int prefetch_layers = 0;
    bool warm_dense = false;
};

// Optional per-token sink (e.g. CSV for benchmarks). The engine calls on_run_info once before the
// first token, then on_token for each token and on_summary at the end of every generation.
class IMetricsSink {
public:
    virtual ~IMetricsSink() = default;
    // Default no-op: a sink that only wants numbers is not obliged to care what produced them.
    virtual void on_run_info(const RunInfo &) {}
    virtual void on_token(const TokenMetrics &) = 0;
    virtual void on_summary(const RunSummary &) = 0;
};

// A sink that appends one CSV row per token to `path` (header written on open).
// Returns nullptr if the file cannot be opened.
IMetricsSink * make_csv_metrics_sink(const std::string & path);

} // namespace bmoe
