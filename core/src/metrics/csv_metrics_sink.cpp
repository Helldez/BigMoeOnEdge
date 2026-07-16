#include "bmoe/metrics.h"

#include <cstdio>

namespace bmoe {

namespace {

class CsvMetricsSink final : public IMetricsSink {
public:
    explicit CsvMetricsSink(std::FILE * f) : f_(f) {
        // New columns are appended LAST so existing positional parsers stay valid: stall_ms (0 when
        // serial), mgmt_ms (cache-management split out of the compute residual), then majflt/cpu_ms
        // (the fault + CPU-time decomposition of what remains of "compute"), then resident_frac
        // (how much of the cache the kernel still has; -1 = unmeasured).
        std::fprintf(f_, "step,steps,wall_ms,io_ms,compute_ms,read_bytes,cache_hit_pct,stall_ms,mgmt_ms,majflt,cpu_ms,"
                         "resident_frac,turn,majflt_mib,cache_budget_mib,rss_mib,rss_anon_mib,rss_file_mib,swap_mib,"
                         "mem_available_mib,mem_free_mib,swap_free_mib\n");
    }
    ~CsvMetricsSink() override {
        if (f_) std::fclose(f_);
    }

    void on_token(const TokenMetrics & m) override {
        std::fprintf(f_,
                     "%d,%d,%.3f,%.3f,%.3f,%llu,%.2f,%.3f,%.3f,%llu,%.3f,%.3f,%d,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,"
                     "%.1f,%.1f,%.1f\n",
                     m.step, m.steps, m.wall_ms, m.io_ms, m.compute_ms, (unsigned long long) m.read_bytes,
                     m.cache_hit_pct, m.stall_ms, m.mgmt_ms, (unsigned long long) m.majflt, m.cpu_ms, m.resident_frac,
                     m.turn, m.majflt_mib, m.cache_budget_mib, m.rss_mib, m.rss_anon_mib, m.rss_file_mib, m.swap_mib,
                     m.mem_available_mib, m.mem_free_mib, m.swap_free_mib);
        std::fflush(f_);
    }
    void on_summary(const RunSummary & s) override {
        // Run-level trailer parsed by scripts/bench-analyze.py as whitespace-separated key=value
        // tokens (order-independent). New keys are appended freely; older parsers ignore unknowns.
        std::fprintf(f_,
                     "# summary tokens=%d s/tok=%.3f tok/s=%.3f read_MiB=%.1f "
                     "io_s=%.2f compute_s/tok=%.3f io_s/tok=%.3f cache_hit_pct=%.1f "
                     "n_prompt=%d load_s=%.3f prefill_s=%.3f prefill_tps=%.2f stall_s/tok=%.3f mgmt_s/tok=%.3f "
                     "cache_resident_MiB=%.1f cache_budget_MiB=%.1f cache_resizes=%lld "
                     "spec_read_MiB=%.1f spec_experts=%lld spec_useful=%lld "
                     "majflt/tok=%.2f cpu_s/tok=%.4f token_demand_MiB=%.1f layer_demand_MiB=%.1f "
                     "cache_cuts=%lld\n",
                     s.n_generated, s.s_per_token, s.tokens_per_second, s.moe_read_mib, s.moe_io_seconds,
                     s.moe_compute_s_per_token, s.moe_io_s_per_token, s.cache_hit_pct, s.n_prompt, s.load_seconds,
                     s.prefill_seconds, s.prefill_seconds > 0 ? s.n_prompt / s.prefill_seconds : 0.0,
                     s.moe_stall_s_per_token, s.moe_mgmt_s_per_token, s.cache_resident_mib, s.cache_budget_mib,
                     s.cache_resizes, s.moe_spec_read_mib, s.moe_spec_experts, s.moe_spec_useful, s.majflt_per_token,
                     s.cpu_s_per_token, s.token_demand_mib, s.layer_demand_mib, s.cache_cuts);
        std::fflush(f_);
    }

private:
    std::FILE * f_ = nullptr;
};

} // namespace

IMetricsSink * make_csv_metrics_sink(const std::string & path) {
    std::FILE * f = std::fopen(path.c_str(), "w");
    return f ? new CsvMetricsSink(f) : nullptr;
}

} // namespace bmoe
