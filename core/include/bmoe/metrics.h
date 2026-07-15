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
    uint64_t majflt = 0;        // major page faults during this decode (backing-store reads)
    double cpu_ms = 0.0;        // CPU time summed across all threads during this decode
    std::string piece;          // text of just this token (delta, for inline streaming)
    std::string text;           // full generated text so far (for UI streaming)
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
    double cache_budget_mib = 0.0; // current cache budget (moves under --cache-mb auto)
    long long cache_resizes = 0;   // runtime budget changes (0 unless auto/explicit resize)

    // Temporal prefetch (zero when --prefetch is off): speculative bytes read during generation,
    // experts successfully prefetched, and how many of those a later routing actually used.
    double moe_spec_read_mib = 0.0;
    long long moe_spec_experts = 0;
    long long moe_spec_useful = 0;
};

// Optional per-token sink (e.g. CSV for benchmarks). The engine calls on_token for each
// token and on_summary once at the end.
class IMetricsSink {
public:
    virtual ~IMetricsSink() = default;
    virtual void on_token(const TokenMetrics &) = 0;
    virtual void on_summary(const RunSummary &) = 0;
};

// A sink that appends one CSV row per token to `path` (header written on open).
// Returns nullptr if the file cannot be opened.
IMetricsSink * make_csv_metrics_sink(const std::string & path);

} // namespace bmoe
