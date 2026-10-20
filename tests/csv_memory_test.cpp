#include "bmoe/log.h"
#include "bmoe/metrics.h"
#include "memory_snapshot.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace bmoe;

static int failures = 0;

static void check(bool condition, const char * name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

static std::vector<std::string> cells(const std::string & row) {
    std::istringstream stream(row);
    std::vector<std::string> result;
    std::string cell;
    while (std::getline(stream, cell, ','))
        result.push_back(cell);
    return result;
}

int main() {
    const std::string path = "csv-memory-test.csv";
    auto sink = make_csv_metrics_sink(path);
    if (!sink) return 1;

    TokenMetrics metrics;
    sink->on_token(metrics); // no RunInfo: the implicit header must carry unknown values too
    pio::ProcessMemory sample;
    sample.process_footprint_known = true;
    sample.process_footprint_bytes = 123 * 1048576ull + 262144;
    sample.process_headroom_known = true;
    sample.process_headroom_bytes = 456 * 1048576ull + 786432;
    sample.rss_bytes = 17 * 1048576ull;
    sample.swap_bytes = 31 * 1048576ull;
    detail::record_process_memory(metrics, sample);
    sink->on_token(metrics);

    sample.process_footprint_bytes = sample.process_headroom_bytes = 0;
    detail::record_process_memory(metrics, sample);
    sink->on_token(metrics);
    sample.process_footprint_known = sample.process_headroom_known = false;
    sample.process_footprint_bytes = sample.process_headroom_bytes = 999;
    detail::record_process_memory(metrics, sample);
    sink->on_token(metrics); // stale bytes and a previously known sample must not look measured
    sink.reset();

    std::ifstream input(path);
    std::string line;
    std::getline(input, line);
    const std::string legacy =
        "step,steps,wall_ms,io_ms,compute_ms,read_bytes,cache_hit_pct,stall_ms,mgmt_ms,majflt,cpu_ms,"
        "dense_resident_frac,turn,majflt_mib,cache_budget_mib,rss_mib,rss_anon_mib,"
        "rss_file_mib,swap_mib,mem_available_mib,mem_free_mib,swap_free_mib,loop_overhead_ms,"
        "mtp_batch,mtp_draft_ms,drain_ms,adopt_ms,ra_issue_ms,ra_wd_ms";
    check(line == legacy + ",process_footprint_mib,process_headroom_mib",
          "new process columns append to the complete existing header");
    const auto header = cells(line);
    std::vector<std::vector<std::string>> rows;
    while (std::getline(input, line)) {
        rows.push_back(cells(line));
        check(rows.back().size() == header.size(), "CSV rows and header have the same width");
    }
    check(rows.size() == 4, "all four memory states are written");
    if (header.size() == 31 && rows.size() == 4 && rows[0].size() == 31 && rows[1].size() == 31 &&
        rows[2].size() == 31 && rows[3].size() == 31) {
        check(rows[0][29] == "-1.000" && rows[0][30] == "-1.000", "default samples serialize as unknown");
        check(rows[1][29] == "123.250" && rows[1][30] == "456.750",
              "footprint and headroom preserve distinct values and units");
        check(rows[1][15] == "17.0" && rows[1][18] == "31.0", "legacy RSS and swap values retain their columns");
        check(rows[2][29] == "0.000" && rows[2][30] == "0.000", "known zero is not replaced by unknown");
        check(rows[3][29] == "-1.000" && rows[3][30] == "-1.000", "unknown flags clear prior measurements");
    }
    input.close();
    std::remove(path.c_str());

    std::string captured;
    set_log_sink([](LogLevel, const char * message, void * user) { *static_cast<std::string *>(user) = message; },
                 &captured);
    detail::log_memory_snapshot("unit-snapshot");
    set_log_sink(nullptr);
    check(captured.find("memory: phase=unit-snapshot process_footprint_mib=") == 0 &&
              captured.find(" process_headroom_mib=") != std::string::npos &&
              captured.find(" system_available_mib=") != std::string::npos,
          "phase snapshots use the existing log sink with named memory counters");
    return failures ? 1 : 0;
}
