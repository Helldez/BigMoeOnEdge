// Serialization regression for the CSV `# summary` trailer (core/src/metrics/csv_metrics_sink.cpp).
//
// The trailer is what scripts/bench-report.sh and the phone app read back, and its prefill keys
// must carry the same raw values BMOE_DONE prints — same names, same units, no normalization.
// This drives the sink directly with a filled RunSummary and asserts the emitted tokens, so a
// renamed key, a dropped field or a unit change fails here instead of in someone's analysis
// script.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "bmoe/metrics.h"

#include <cstdio>
#include <fstream>
#include <string>

using namespace bmoe;

static int failures = 0;

// The trailer is whitespace-separated key=value tokens; asserting the exact token also pins the
// printf precision, which is part of the contract (values must round the way BMOE_DONE rounds).
static void expect_key(const std::string & line, const char * key, const char * wanted) {
    const std::string pat = std::string(key) + "=" + wanted;
    if (line.find(pat) != std::string::npos) {
        std::printf("[PASS] summary %s\n", key);
    } else {
        std::printf("[FAIL] summary %s\n  wanted token '%s' in:\n  %s\n", key, pat.c_str(), line.c_str());
        ++failures;
    }
}

int main(int argc, char ** argv) {
    // Next to the test binary's invocation, in the test working directory: parallel ctest runs
    // each get their own file and nothing is left behind.
    const std::string out = argc > 1 ? argv[1] : "csv-summary-test.csv";
    IMetricsSink * sink = make_csv_metrics_sink(out);
    if (!sink) {
        std::printf("[FAIL] could not open %s for writing\n", out.c_str());
        return 1;
    }
    RunInfo r; // defaults: the preamble is not what this test is about
    sink->on_run_info(r);

    RunSummary s;                     // value-init: every other field zero, only the five under test are set
    s.prefill_cpu_seconds = 12.3456;  // %.3f -> 12.346
    s.prefill_read_mib = 4096.5;      // %.1f -> 4096.5
    s.prefill_io_seconds = 3.4567;    // %.3f -> 3.457
    s.prefill_stall_seconds = 2.7182; // %.3f -> 2.718
    s.prefill_mgmt_seconds = 1.4142;  // %.3f -> 1.414
    sink->on_summary(s);
    delete sink; // the destructor closes and flushes the file

    std::ifstream in(out);
    std::string line, summary;
    while (std::getline(in, line))
        if (line.rfind("# summary", 0) == 0) summary = line;
    if (summary.empty()) {
        std::printf("[FAIL] no '# summary' line in %s\n", out.c_str());
        ++failures;
    } else {
        expect_key(summary, "prefill_cpu_s", "12.346");
        expect_key(summary, "prefill_read_mib", "4096.5");
        expect_key(summary, "prefill_io_s", "3.457");
        expect_key(summary, "prefill_stall_s", "2.718");
        expect_key(summary, "prefill_mgmt_s", "1.414");
    }
    std::remove(out.c_str());
    return failures ? 1 : 0;
}
