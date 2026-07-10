#include "bmoe/metrics.h"

#include <cstdio>

namespace bmoe {

namespace {

class CsvMetricsSink final : public IMetricsSink {
public:
    explicit CsvMetricsSink(std::FILE * f) : f_(f) {
        std::fprintf(f_, "step,steps,wall_ms,io_ms,compute_ms,read_bytes,cache_hit_pct\n");
    }
    ~CsvMetricsSink() override {
        if (f_) std::fclose(f_);
    }

    void on_token(const TokenMetrics & m) override {
        std::fprintf(f_, "%d,%d,%.3f,%.3f,%.3f,%llu,%.2f\n", m.step, m.steps, m.wall_ms, m.io_ms, m.compute_ms,
                     (unsigned long long) m.read_bytes, m.cache_hit_pct);
        std::fflush(f_);
    }
    void on_summary(const RunSummary & s) override {
        std::fprintf(f_,
                     "# summary tokens=%d s/tok=%.3f tok/s=%.3f read_MiB=%.1f "
                     "io_s=%.2f compute_s/tok=%.3f io_s/tok=%.3f cache_hit_pct=%.1f\n",
                     s.n_generated, s.s_per_token, s.tokens_per_second, s.moe_read_mib, s.moe_io_seconds,
                     s.moe_compute_s_per_token, s.moe_io_s_per_token, s.cache_hit_pct);
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
