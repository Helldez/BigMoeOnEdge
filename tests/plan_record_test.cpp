// A plan's inputs as text (tools/planlab/plan_record.h): the text reads back as what was written, a record
// replays to the plan it was taken from, and every record kept in tests/plan-records - each one a
// machine somebody ran `bmoe-planlab record` on - still makes the plan it made there.
//
// The last part is the point. A rule change is tried here against every machine on record, on every
// build, and a plan that moves on one of them fails with both lines printed: either the change was
// not meant to reach that machine, or the record is replayed and written back in the same change
// (docs/plan-records.md), where a reviewer reads the move.

#include "bmoe/params.h"
#include "plan_record.h"
#include "bmoe/planner.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace bmoe;

static int failures = 0;

static void check(bool cond, const std::string & name, const std::string & detail = "") {
    if (cond) {
        std::printf("[PASS] %s\n", name.c_str());
    } else {
        std::printf("[FAIL] %s%s%s\n", name.c_str(), detail.empty() ? "" : " - ", detail.c_str());
        ++failures;
    }
}

static const uint64_t KiB = 1024ull;
static const uint64_t MiB = 1024ull * 1024ull;
static const uint64_t GiB = 1024ull * 1024ull * 1024ull;

// A model several times the machine below, so the plan streams and every sizing rule runs.
static ModelProfile model() {
    ModelProfile m;
    m.arch = "synthetic-moe";
    m.is_moe = true;
    m.n_layer = 48;
    m.n_moe_layer = 48;
    for (uint32_t i = 0; i < 48; ++i)
        m.moe_layers.push_back(i);
    m.n_expert = 256;
    m.n_expert_used = 8;
    m.n_embd = 2048;
    m.n_expert_projections = 3;
    m.expert_slice_bytes = 530432;
    m.token_cycle_bytes = (uint64_t) 48 * 8 * 3 * 530432;
    m.file_bytes = 21 * GiB;
    m.expert_bytes = 18 * GiB;
    m.dense_bytes = 3 * GiB;
    m.largest_dense_tensor = 243 * MiB;
    m.largest_expert_layer_bytes = 384 * MiB;
    m.largest_layer_dense_bytes = 40 * MiB;
    m.embedding_tensors = {"token_embd.weight"};
    m.expert_file = "model.gguf";
    m.expert_type_id = 12;
    m.expert_type_ids = {12, 14};
    m.group(WeightGroup::Attention) = {1400 * MiB, 1400 * MiB, false, false};
    m.group(WeightGroup::DenseFfn) = {400 * MiB, 400 * MiB, false, false};
    m.group(WeightGroup::Output) = {700 * MiB, 700 * MiB, false, false};
    m.group(WeightGroup::Other) = {228 * MiB, 228 * MiB, false, false};
    m.group(WeightGroup::Embedding) = {344 * MiB, 8 * KiB, true, true};
    m.group(WeightGroup::Experts) = {18 * GiB, m.token_cycle_bytes, false, true};
    m.hit_curve.measured = true;
    m.hit_curve.decode_tokens = 200;
    m.hit_curve.prefill_tokens = 40;
    m.hit_curve.points = {{0, 0.0}, {1 * GiB, 0.41}, {4 * GiB, 0.78}, {18 * GiB, 1.0}};
    m.knobs.hardware = "0123456789abcdef";
    m.knobs.engine = "0.0.0";
    m.knobs.threads = 6;
    m.knobs.threads_tried = {8, 6, 4};
    m.knobs.overlap = 1;
    m.ok = true;
    return m;
}

static HardwareProfile machine() {
    HardwareProfile h;
    h.label = "8 cores, 12288 MiB";
    h.residency_budget = 7 * GiB;
    h.memory_total = 12 * GiB;
    h.anon_overflow = Overflow::Compress;
    h.holdable_bytes = 8 * GiB;
    h.holdable_from = Headroom::Compressible;
    h.reclaim_exempt_max = 2047 * MiB;
    h.lockable_bytes = ~0ull;
    h.lock_in_place_bytes = 64 * KiB;
    h.n_cores = 8;
    h.core_classes = {1, 3, 4};
    h.best_threads = 6;
    h.thread_rates = {{4, 9.5}, {6, 12.25}, {8, 11.0}};
    h.host_bandwidth_gibs = 12.25;
    h.host_expert_gibs = 9.125;
    h.host_dense_gibs = 10.5;
    h.backends_looked = true;
    h.backends_linked = 2;
    h.backends_placeable = 1;
    h.storage.align = 4096;
    h.storage.direct_ok = Tri::Yes;
    h.storage.mapping_serialises_reads = Tri::No;
    h.storage.rate_curve = {{4 * (uint32_t) KiB, 1, 31.3},
                            {256 * (uint32_t) KiB, 1, 901.0},
                            {256 * (uint32_t) KiB, 2, 1600.0},
                            {512 * (uint32_t) KiB, 4, 2201.5}};
    h.storage.refault_mibs = 23.0;
    h.storage.rate_spread = 1.07;
    ComputeDevice cpu;
    cpu.name = "CPU";
    cpu.is_cpu = true;
    cpu.shares_host_memory = Tri::Yes;
    cpu.rebindable = true;
    h.devices.push_back(cpu);
    ComputeDevice gpu;
    gpu.name = "integrated";
    gpu.description = "a device whose description has = and , and a \\ in it";
    gpu.memory_total = 6 * GiB;
    gpu.memory_free = 5 * GiB;
    gpu.shares_host_memory = Tri::Yes;
    gpu.usable = Tri::No;
    gpu.unusable_reason = "the probe ended:\nsignal 11";
    gpu.probed_out_of_process = true;
    gpu.memory_bandwidth_gibs = 20.0;
    gpu.split_seconds = 0.00042;
    h.devices.push_back(gpu);
    return h;
}

static Placement placement(uint32_t width) {
    Placement p;
    p.fitted = true;
    p.outcome = "no device with memory of its own";
    p.n_ctx = 8192;
    for (uint32_t i = 0; i < 48; ++i)
        p.host_expert_layers.push_back(i);
    p.host_resident_bytes = 3 * GiB + 600 * MiB;
    p.raw_host_model_bytes = 3 * GiB;
    p.raw_host_context_bytes = 400 * MiB;
    p.raw_host_compute_bytes = 200 * MiB;
    p.compute_width = width;
    p.host_compute_per_token = 96 * KiB;
    p.override_patterns = {"blk\\.(0|1)\\.ffn_.*_exps=CPU"};
    p.device_split = {{"integrated", 0}};
    p.device_spare = {{"integrated", 123 * MiB}};
    return p;
}

static std::string read_file(const std::filesystem::path & p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream all;
    all << f.rdbuf();
    return all.str();
}

int main(int argc, char ** argv) {
    // ── a record made from a plan ────────────────────────────────────────────────────────
    RunConfig base;
    base.moe.enabled = true;
    base.n_ctx = 8192;
    base.model_path = "/somewhere/private/model.gguf";
    PlanRequest req;
    req.pinned = {"moe-stream", "ctx-size"};
    const HardwareProfile hw = machine();
    const ModelProfile mp = model();
    AutoPlan facts;
    facts.hw = hw;
    facts.model = mp;
    facts.placement = placement((uint32_t) base.graph_width());
    facts.refit = [](const AutoPlan::FitKey & key) { return placement(key.width); };
    const Plan plan = replan(facts, base, req, mp.knobs);
    const std::vector<PlacedAt> asked = placements_of(facts, base);
    PlanRecord rec = make_plan_record(base, req, hw, mp, asked, plan);
    rec.note = "a synthetic machine";
    set_fact(rec.observed, "generated_tokens", 128ll);
    set_fact(rec.observed, "seconds_per_token", 0.2125);
    set_fact(rec.observed, "cache_hit_decode_pct", 71.5);
    rec.env["os"] = "an operating system";
    rec.env["swap_kind"] = "zram";

    const std::string text = format_plan_record(rec);
    check(text.find("/somewhere/private") == std::string::npos, "a record carries no local path");
    check(!asked.empty() && plan.config.moe.enabled, "the synthetic plan streams", plan.decline_reason);

    PlanRecord back;
    std::string err;
    std::vector<std::string> unknown;
    check(parse_plan_record(text, back, err, &unknown), "a record's text is read back", err);
    check(unknown.empty(), "every key written is a key read", unknown.empty() ? "" : unknown.front());
    check(format_plan_record(back) == text, "read back, it writes the same text");
    check(back.hw.devices.size() == 2 && back.hw.devices[1].unusable_reason == "the probe ended:\nsignal 11" &&
              back.hw.devices[1].description == hw.devices[1].description,
          "a string keeps its line break, its backslash and its punctuation");
    check(back.placements.size() == asked.size() &&
              back.placements[0].placement.override_patterns == asked[0].placement.override_patterns,
          "an override pattern survives");
    check(back.model.knobs.threads == 6 && back.model.knobs.threads_tried.size() == 3 && back.model.knobs.overlap == 1,
          "what an earlier load measured is carried");
    check(back.ran() && fact(back.observed, "seconds_per_token") == 0.2125 && back.env["swap_kind"] == "zram" &&
              fact(back.predicted, "seconds_per_token") == plan.allocation.seconds_per_token &&
              back.predicted.count("group.experts.seconds_per_token") == 1,
          "the three tables come back: where it ran, the prediction in parts, what the run measured");
    check(back.hw.lockable_bytes == ~0ull, "the largest value there is reads back as itself");

    // ── replayed, it is the plan it was taken from ───────────────────────────────────────
    std::vector<std::string> problems;
    const RunConfig cfg = record_config(back, problems);
    check(problems.empty(), "the recorded configuration is this build's", problems.empty() ? "" : problems.front());
    const Replay same = replay_plan(back, cfg, back.request);
    check(resolved_line(same.plan) == rec.resolved, "a record replays to the plan it recorded",
          rec.resolved + "  vs  " + resolved_line(same.plan));
    check(same.caveats.empty(), "and stands on recorded facts alone", same.caveats.empty() ? "" : same.caveats.front());
    check(same.plan.allocation.seconds_per_token == plan.allocation.seconds_per_token,
          "with the prediction it made, to the last digit");

    // ── a fact changed is one appended line ──────────────────────────────────────────────
    PlanRecord smaller;
    check(parse_plan_record(text + "hw.residency_budget=" + std::to_string(4 * GiB) + "\nhw.holdable_bytes=0\n" +
                                "hw.no_such_fact=1\n",
                            smaller, err, &unknown),
          "a record with facts appended is read", err);
    check(smaller.hw.residency_budget == 4 * GiB && smaller.hw.holdable_bytes == 0, "the last value of a key wins");
    check(unknown.size() == 1 && unknown[0] == "hw.no_such_fact", "a key that is not a fact is reported");
    const Replay tight = replay_plan(smaller, cfg, smaller.request);
    check(tight.plan.allocation.cache_bytes < same.plan.allocation.cache_bytes,
          "less memory on the same machine is a smaller cache",
          std::to_string(tight.plan.allocation.cache_bytes) + " vs " +
              std::to_string(same.plan.allocation.cache_bytes));

    // ── a knob pinned at replay is the operator's, there as here ─────────────────────────
    {
        RunConfig pinned_cfg = cfg;
        PlanRequest pinned_req = back.request;
        std::string e;
        find_param("cache-mb")->set(pinned_cfg, "1500", e);
        pinned_req.pinned.push_back("cache-mb");
        const Replay r = replay_plan(back, pinned_cfg, pinned_req);
        check(r.plan.config.moe.cache_mb == 1500, "a knob pinned at replay is kept",
              std::to_string(r.plan.config.moe.cache_mb));
    }

    // ── a width the fitter was never asked at is said, not invented ──────────────────────
    {
        RunConfig narrow = cfg;
        narrow.n_ubatch = 1024;
        PlanRequest r2 = back.request;
        r2.pinned.push_back("ubatch");
        const Replay r = replay_plan(back, narrow, r2);
        check(!r.caveats.empty(), "a placement moved along its slope is a caveat");
    }
    {
        RunConfig longer = cfg;
        longer.n_ctx = 32768;
        const Replay r = replay_plan(back, longer, back.request);
        check(!r.caveats.empty(), "a context the placement was not made for is a caveat");
    }

    // ── damaged text is refused with the reason ──────────────────────────────────────────
    PlanRecord bad;
    check(!parse_plan_record("something else\n", bad, err), "text without the header is not a record");
    check(!parse_plan_record(text + "hw.n_cores=many\n", bad, err) && err.find("hw.n_cores") != std::string::npos,
          "a value that is not its type names its key", err);

    // ── every machine on record ──────────────────────────────────────────────────────────
    if (argc > 1) {
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (const auto & e : std::filesystem::directory_iterator(argv[1], ec))
            if (e.path().extension() == ".planrec") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        std::printf("%zu machine(s) on record in %s\n", files.size(), argv[1]);
        for (const auto & f : files) {
            const std::string name = f.filename().string();
            PlanRecord r;
            if (!parse_plan_record(read_file(f), r, err, &unknown)) {
                check(false, name + ": is a plan record", err);
                continue;
            }
            check(unknown.empty(), name + ": every key is a fact this build reads",
                  unknown.empty() ? "" : unknown.front());
            problems.clear();
            const RunConfig c = record_config(r, problems);
            check(problems.empty(), name + ": its configuration is this build's",
                  problems.empty() ? "" : problems.front());
            const Replay again = replay_plan(r, c, r.request);
            const std::string now = resolved_line(again.plan);
            check(now == r.resolved, name + ": still makes the plan it recorded",
                  "\n    recorded: " + r.resolved + "\n    now:      " + now +
                      "\n    if the change is meant: bmoe-planlab replay FILE -o FILE");
            if (r.ran() && again.plan.allocation.seconds_per_token > 0.0)
                std::printf(
                    "       %s: predicts %.3f s/token, its run measured %.3f (%+.0f%%)\n", name.c_str(),
                    again.plan.allocation.seconds_per_token, fact(r.observed, "seconds_per_token"),
                    100.0 * (again.plan.allocation.seconds_per_token / fact(r.observed, "seconds_per_token") - 1.0));
        }
    }

    // ── the data set: every record kept in docs/bench-data/plan-records ──────────────────
    // Not held to the plan it recorded - adding a row must never block a rule change - only to
    // being a record this build reads and plans from.
    if (argc > 2) {
        size_t n = 0;
        std::error_code ec;
        for (const auto & e : std::filesystem::recursive_directory_iterator(argv[2], ec)) {
            if (e.path().extension() != ".planrec") continue;
            ++n;
            const std::string name = e.path().filename().string();
            PlanRecord r;
            if (!parse_plan_record(read_file(e.path()), r, err, &unknown)) {
                check(false, name + ": is a plan record", err);
                continue;
            }
            problems.clear();
            const Replay again = replay_plan(r, record_config(r, problems), r.request);
            check(problems.empty() && unknown.empty(), name + ": reads and plans on this build",
                  !problems.empty() ? problems.front()
                  : unknown.empty() ? ""
                                    : "unknown key " + unknown.front());
            if (r.ran() && again.plan.allocation.seconds_per_token > 0.0)
                std::printf("       %s: predicts %.3f s/token, its run measured %.3f\n", name.c_str(),
                            again.plan.allocation.seconds_per_token, fact(r.observed, "seconds_per_token"));
        }
        std::printf("%zu record(s) in the data set %s\n", n, argv[2]);
    }

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
