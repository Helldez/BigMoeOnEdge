// bmoe-planlab: the planner's bench (docs/plan-records.md). A developer's instrument, built with
// -DBMOE_BUILD_TOOLS=ON and carried by no release.
//
//   bmoe-planlab record -m MODEL -o FILE [--note TEXT] [--routing-dir DIR] [--run PROMPT] [flags]
//       On the machine: probe it and the model exactly as `bmoe-cli --auto` does, plan, and write
//       everything the plan was made from to FILE. With --run, also run the plan on PROMPT and add
//       what the run measured.
//
//   bmoe-planlab errors DIR / table DIR [-o CSV]
//       Every record under DIR together: the cost model's error on each, and all of them as one
//       table for whatever is fitted on it.
//
//   bmoe-planlab simulate MACHINE -m MODEL -o FILE [flags]
//       Anywhere: plan for a machine described by MACHINE (a host and cards taken from records,
//       sized to the ones asked about), with llama.cpp's own fitter placing MODEL on simulated
//       cards. Writes a record of it, which replay and the rest then read (docs/hardware-simulation.md).
//
//   bmoe-planlab replay FILE [--set KEY=VALUE]... [--sweep PARAM=V1,V2,...] [-o FILE] [flags]
//       Anywhere: plan again from FILE, with no model, no device and no drive.
//
// `flags` are engine parameters, spelled as bmoe-cli spells them (the parameter table); each one
// typed is pinned, there as here.

#include "env_probe.h"
#include "plan_record.h"
#include "sim_parts.h"
#include "sim_speed.h"
#include "simulate.h"

#include "bmoe/auto_plan.h"
#include "bmoe/measured_knobs.h"
#include "bmoe/params.h"
#include "bmoe/probe.h"
#include "bmoe/routing.h"
#include "bmoe/runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace bmoe;

namespace {

int usage() {
    std::fputs("usage:\n"
               "  bmoe-planlab record -m MODEL -o FILE [--note TEXT] [--routing-dir DIR] [--run PROMPT]\n"
               "                      [--no-probe-io] [engine flags]\n"
               "  bmoe-planlab replay FILE [--set KEY=VALUE]... [--sweep PARAM=V1,V2,...] [-o FILE]\n"
               "                      [engine flags]\n"
               "  bmoe-planlab simulate MACHINE -m MODEL -o FILE [engine flags]\n"
               "  bmoe-planlab parts CATALOG       the model of parts, and each machine left out of it\n"
               "  bmoe-planlab speed DIR           the tool's decode estimate against every measured run\n"
               "  bmoe-planlab errors DIR          predicted against measured, every record with a run\n"
               "  bmoe-planlab table DIR [-o CSV]  every record as one row, every key a column\n"
               "docs/plan-records.md\n",
               stderr);
    return 2;
}

bool read_text_file(const std::string & path, std::string & text) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    std::ostringstream all;
    all << f.rdbuf();
    text = all.str();
    return true;
}

bool write_text_file(const std::string & path, const std::string & text) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << text;
    return (bool) f;
}

// What both commands share: the tool's own options, and every other argument an engine parameter.
struct Args {
    RunConfig cfg;
    std::set<std::string> seen; // the parameters typed, by key
    std::string file, out, note, routing_dir, prompt, sweep, probe_device;
    std::vector<std::string> sets;
    bool probe_io = true;
    bool run = false;
    bool ok = true;
};

Args parse_args(int argc, char ** argv, int first, bool takes_file) {
    Args a;
    for (int i = first; i < argc; ++i) {
        const std::string s = argv[i];
        const char * value = i + 1 < argc ? argv[i + 1] : nullptr;
        auto take = [&](std::string & dst) {
            if (!value) {
                std::fprintf(stderr, "bmoe-planlab: missing value for %s\n", s.c_str());
                a.ok = false;
                return;
            }
            dst = value;
            ++i;
        };
        if (s == "-o")
            take(a.out);
        else if (s == "--note")
            take(a.note);
        else if (s == "--routing-dir")
            take(a.routing_dir);
        else if (s == "--run") {
            take(a.prompt);
            a.run = true;
        } else if (s == "--sweep")
            take(a.sweep);
        else if (s == "--set") {
            std::string kv;
            take(kv);
            a.sets.push_back(kv);
        } else if (s == "--probe-device")
            take(a.probe_device);
        else if (s == "--no-probe-io")
            a.probe_io = false;
        else if (takes_file && a.file.empty() && !s.empty() && s[0] != '-')
            a.file = s;
        else {
            const FlagResult r = apply_flag(a.cfg, s.c_str(), value);
            if (!r.matched || !r.error.empty()) {
                std::fprintf(stderr, "bmoe-planlab: %s\n",
                             r.matched ? r.error.c_str() : ("unknown argument " + s).c_str());
                a.ok = false;
                return a;
            }
            if (r.consumed_value) ++i;
            a.seen.insert(r.param->key);
        }
    }
    return a;
}

// ── what a run measured, as facts ───────────────────────────────────────────────────────────
// A column of the per-token metrics as a distribution. The mean alone hides what a predictor has to
// learn from: a decode whose first tokens fill the cache, a tail of tokens that waited on the drive.
void spread(Facts & o, const std::string & key, std::vector<double> v) {
    if (v.empty()) return;
    // The second half is the decode once the cache has settled, which is what a plan predicts.
    std::vector<double> settled(v.begin() + (long) (v.size() / 2), v.end());
    auto at = [](std::vector<double> & x, double q) {
        std::sort(x.begin(), x.end());
        return x[(size_t) (q * (double) (x.size() - 1) + 0.5)];
    };
    double sum = 0.0;
    for (double d : v)
        sum += d;
    set_fact(o, key + ".mean", sum / (double) v.size());
    set_fact(o, key + ".p10", at(v, 0.10));
    set_fact(o, key + ".p50", at(v, 0.50));
    set_fact(o, key + ".p90", at(v, 0.90));
    set_fact(o, key + ".max", v.back());
    set_fact(o, key + ".settled_p50", at(settled, 0.50));
}

void observe(Facts & o,
             const RunSummary & s,
             const std::vector<TokenMetrics> & tokens,
             const Facts & before,
             const Facts & after) {
    // The run as the engine summed it up.
    set_fact(o, "generated_tokens", (long long) s.n_generated);
    set_fact(o, "seconds_per_token", s.s_per_token);
    set_fact(o, "load_seconds", s.load_seconds);
    set_fact(o, "prompt_tokens", (long long) s.n_prompt);
    set_fact(o, "prefill_seconds", s.prefill_seconds);
    set_fact(o, "prefill_cpu_seconds", s.prefill_cpu_seconds);
    set_fact(o, "prefill_read_mib", s.prefill_read_mib);
    set_fact(o, "prefill_io_seconds", s.prefill_io_seconds);
    set_fact(o, "prefill_stall_seconds", s.prefill_stall_seconds);
    set_fact(o, "prefill_device_tokens", (long long) s.prefill_device_tokens);
    set_fact(o, "compute_s_per_token", s.moe_compute_s_per_token);
    set_fact(o, "io_s_per_token", s.moe_io_s_per_token);
    set_fact(o, "mgmt_s_per_token", s.moe_mgmt_s_per_token);
    set_fact(o, "stall_s_per_token", s.moe_stall_s_per_token);
    set_fact(o, "flash_wait_s_per_token", s.moe_flash_wait_s_per_token);
    set_fact(o, "loop_overhead_s_per_token", s.loop_overhead_s_per_token);
    set_fact(o, "cpu_s_per_token", s.cpu_s_per_token);
    set_fact(o, "majflt_per_token", s.majflt_per_token);
    set_fact(o, "read_mib", s.moe_read_mib);
    set_fact(o, "token_demand_mib", s.token_demand_mib);
    set_fact(o, "cache_hit_pct", s.cache_hit_pct);
    set_fact(o, "cache_hit_decode_pct", s.cache_hit_decode_pct);
    set_fact(o, "cache_budget_mib", s.cache_budget_mib);
    set_fact(o, "cache_resident_mib", s.cache_resident_mib);
    set_fact(o, "cache_resizes", s.cache_resizes);
    set_fact(o, "cache_evictions", s.cache_evictions);
    set_fact(o, "cache_rereads", s.cache_rereads);
    set_fact(o, "cache_discarded", s.cache_discarded);
    set_fact(o, "cache_discarded_on_hit", s.cache_discarded_on_hit);
    set_fact(o, "experts_routed", s.experts_routed);

    // The same decode token by token: time and its parts, then memory as the run moved through it.
    const std::pair<const char *, std::function<double(const TokenMetrics &)>> columns[] = {
        {"token.wall_ms", [](const TokenMetrics & m) { return m.wall_ms; }},
        {"token.compute_ms", [](const TokenMetrics & m) { return m.compute_ms; }},
        {"token.io_ms", [](const TokenMetrics & m) { return m.io_ms; }},
        {"token.stall_ms", [](const TokenMetrics & m) { return m.stall_ms; }},
        {"token.mgmt_ms", [](const TokenMetrics & m) { return m.mgmt_ms; }},
        {"token.flash_wait_ms", [](const TokenMetrics & m) { return m.flash_wait_ms; }},
        {"token.cpu_ms", [](const TokenMetrics & m) { return m.cpu_ms; }},
        {"token.read_mib", [](const TokenMetrics & m) { return (double) m.read_bytes / (1024.0 * 1024.0); }},
        {"token.majflt", [](const TokenMetrics & m) { return (double) m.majflt; }},
    };
    for (const auto & c : columns) {
        std::vector<double> v;
        for (const TokenMetrics & m : tokens)
            v.push_back(c.second(m));
        spread(o, c.first, v);
    }
    if (!tokens.empty()) {
        const TokenMetrics & first = tokens.front();
        const TokenMetrics & last = tokens.back();
        double rss_peak = 0, swap_peak = 0, avail_min = first.mem_available_mib, swap_free_min = first.swap_free_mib;
        double footprint_peak = -1, headroom_min = first.process_headroom_mib, dense_min = first.dense_resident_frac;
        double budget_min = first.cache_budget_mib;
        for (const TokenMetrics & m : tokens) {
            rss_peak = (std::max)(rss_peak, m.rss_mib);
            swap_peak = (std::max)(swap_peak, m.swap_mib);
            avail_min = (std::min)(avail_min, m.mem_available_mib);
            swap_free_min = (std::min)(swap_free_min, m.swap_free_mib);
            footprint_peak = (std::max)(footprint_peak, m.process_footprint_mib);
            headroom_min = (std::min)(headroom_min, m.process_headroom_mib);
            dense_min = (std::min)(dense_min, m.dense_resident_frac);
            budget_min = (std::min)(budget_min, m.cache_budget_mib);
        }
        set_fact(o, "memory.rss_mib.peak", rss_peak);
        set_fact(o, "memory.rss_mib.last", last.rss_mib);
        set_fact(o, "memory.rss_anon_mib.last", last.rss_anon_mib);
        set_fact(o, "memory.rss_file_mib.last", last.rss_file_mib);
        // The process's own memory the system compressed or swapped: what the plan meant to hold and lost.
        set_fact(o, "memory.process_swap_mib.peak", swap_peak);
        set_fact(o, "memory.process_swap_mib.last", last.swap_mib);
        set_fact(o, "memory.available_mib.first", first.mem_available_mib);
        set_fact(o, "memory.available_mib.min", avail_min);
        set_fact(o, "memory.swap_free_mib.first", first.swap_free_mib);
        set_fact(o, "memory.swap_free_mib.min", swap_free_min);
        set_fact(o, "memory.process_footprint_mib.peak", footprint_peak);
        set_fact(o, "memory.process_headroom_mib.min", headroom_min);
        set_fact(o, "memory.dense_resident_frac.min", dense_min);
        // A budget that ended under where it began is the cache brake, or a lock granted part way.
        set_fact(o, "cache_budget_mib.first", first.cache_budget_mib);
        set_fact(o, "cache_budget_mib.min", budget_min);
    }

    // The system around the run: each counter before it and after it.
    for (const auto & e : before)
        o["system." + e.first + ".before"] = e.second;
    for (const auto & e : after)
        o["system." + e.first + ".after"] = e.second;
}

// ── record ──────────────────────────────────────────────────────────────────────────────────
int record_main(const Args & a) {
    RunConfig cfg = a.cfg;
    if (cfg.model_path.empty()) return usage();

    // The child of an isolated device probe (bmoe/auto_plan.h): one device, its facts, nothing else.
    // Before anything the parent's command needs: the child is started with the model and the
    // device alone, and a child that stops at a missing -o is read as a device that failed.
    if (!a.probe_device.empty()) {
        std::string line;
        if (!probe_device_child(cfg.model_path.c_str(), a.probe_device, line)) return 3;
        std::printf("%s\n", line.c_str());
        return 0;
    }
    if (a.out.empty()) return usage();

    PlanRequest req;
    req.pinned.assign(a.seen.begin(), a.seen.end());
    AutoPlanOptions opt;
    opt.probe_io = a.probe_io;
    const std::string model = cfg.model_path;
    opt.isolated_probe_args = [model](const std::string & dev) {
        return std::vector<std::string>{"record", "-m", model, "--probe-device", dev};
    };
    opt.on_step = [](const char * step, int index, int count) {
        std::fprintf(stderr, "planlab: probing %s (%d/%d)\n", step, index + 1, count);
    };
    // What bmoe-cli kept for this model with the same --routing-dir: its routing and its measured
    // knobs. Read, never written: a record is of the plan a front-end would make, not a new load.
    RoutingTrace routing;
    std::vector<MeasuredKnobs> knobs;
    if (!a.routing_dir.empty()) {
        const std::string fp = model_fingerprint(model.c_str());
        const std::filesystem::path dir = std::filesystem::u8path(a.routing_dir);
        std::string text, err;
        if (!fp.empty() && read_text_file((dir / (fp + ".routing")).u8string(), text) &&
            parse_routing_trace(text, routing, err))
            opt.routing = &routing;
        if (!fp.empty() && read_text_file((dir / (fp + ".knobs")).u8string(), text) &&
            parse_measured_knobs(text, knobs, err))
            opt.knobs = &knobs;
    }

    const AutoPlan planned = probe_and_plan(cfg, req, opt);
    PlanRecord rec = make_plan_record(cfg, req, planned.hw, planned.model, placements_of(planned, cfg), planned.plan);
    rec.note = a.note;
    probe_environment(rec.env);
    rec.env["model_file"] = std::filesystem::u8path(model).filename().u8string();
    rec.env["model_fingerprint"] = model_fingerprint(model.c_str());
    {
        char day[16] = "";
        const std::time_t now = std::time(nullptr);
        if (const std::tm * utc = std::gmtime(&now)) std::strftime(day, sizeof(day), "%Y-%m-%d", utc);
        rec.env["recorded_utc"] = day;
    }
    std::fputs(planned.plan.explain().c_str(), stdout);
    // A device its probe lost is left out of the plan, and the record is then of a machine without
    // it. That is a finding when the driver is at fault and a wasted record when the tool is, so it
    // is said where it cannot be missed.
    for (const ComputeDevice & d : planned.hw.devices)
        if (d.usable == Tri::No)
            std::fprintf(stderr, "planlab: WARNING: device %s was left out of this plan: %s\n", d.name.c_str(),
                         d.unusable_reason.c_str());

    // Replayed once before it is written: a record that does not make the plan it was taken from is
    // missing a fact, and the place to learn that is the machine that still has it.
    {
        PlanRecord back;
        std::string err;
        std::vector<std::string> problems;
        const bool read = parse_plan_record(format_plan_record(rec), back, err);
        const std::string again =
            read ? resolved_line(replay_plan(back, record_config(back, problems), back.request).plan) : err;
        if (again != rec.resolved || !problems.empty())
            std::fprintf(stderr,
                         "planlab: WARNING: the record does not replay to this plan\n  planned:  %s\n  replayed: %s\n",
                         rec.resolved.c_str(), again.c_str());
    }
    if (!write_text_file(a.out, format_plan_record(rec))) {
        std::fprintf(stderr, "bmoe-planlab: cannot write %s\n", a.out.c_str());
        return 1;
    }
    std::fprintf(stderr, "planlab: record written to %s\n", a.out.c_str());
    if (!a.run) return 0;

    // The plan's own run, for the prediction to stand beside. Its per-token metrics go to a CSV
    // beside the record, always: the record keeps their distribution, the CSV keeps every token.
    cfg = planned.plan.config;
    cfg.prompt = a.prompt;
    const std::unique_ptr<IMetricsSink> csv = make_csv_metrics_sink(a.out + ".csv");
    std::vector<TokenMetrics> tokens;
    RunOutputs outs;
    outs.sink = csv.get();
    outs.on_token = [&tokens](const TokenMetrics & m) {
        tokens.push_back(m);
        tokens.back().piece.clear(); // the answer is not a measurement, and is not kept
        tokens.back().text.clear();
        tokens.back().reasoning.clear();
    };
    Facts before, after;
    sample_system(before);
    const RunResult r = run(cfg, outs);
    sample_system(after);
    if (!r) {
        std::fprintf(stderr, "bmoe-planlab: the run failed: %s\n", r.error.c_str());
        return 1;
    }
    observe(rec.observed, r.summary, tokens, before, after);
    if (!write_text_file(a.out, format_plan_record(rec))) return 1;
    const double predicted = fact(rec.predicted, "seconds_per_token");
    std::printf(
        "planlab: predicted %.3f s/token, measured %.3f over %d tokens (%zu facts observed, tokens in %s.csv)\n",
        predicted, r.summary.s_per_token, r.summary.n_generated, rec.observed.size(), a.out.c_str());
    return 0;
}

// ── the decode estimate (sim_speed.h) ───────────────────────────────────────────────────────
// The fitter's answer a plan was made on: the one at the plan's own width and context.
const Placement & placement_for(const PlanRecord & rec, const Plan & plan) {
    static const Placement none;
    const PlacedAt * best = nullptr;
    for (const PlacedAt & p : rec.placements) {
        if (!best) best = &p;
        if (p.n_ubatch == (uint32_t) plan.config.graph_width() &&
            (p.n_ctx == 0 || p.n_ctx == (uint32_t) plan.config.n_ctx))
            best = &p;
    }
    return best ? best->placement : none;
}

void print_estimate(const char * who, const DecodeEstimate & e) {
    if (e.seconds <= 0.0) {
        std::printf("%s: no decode estimate: %s\n", who, e.why.c_str());
        return;
    }
    std::printf("%s: decode estimate %.1f ms/token (%.1f tok/s), a sum of measured rates:\n", who, 1e3 * e.seconds,
                1.0 / e.seconds);
    for (const auto & [name, s] : e.parts)
        std::printf("  %-50s %8.2f ms\n", name.c_str(), 1e3 * s);
    if (e.device_cache_hit >= 0.0)
        std::printf("  device cache serves %.0f%% of the host's experts (%s)\n", 100.0 * e.device_cache_hit,
                    e.hit_from_routing ? "the model's own routing" : "the fraction it holds: no routing given");
}

// ── replay ──────────────────────────────────────────────────────────────────────────────────
// One line of a replay: what the plan holds and what it predicts, for a table of several.
void print_row(const std::string & what, const Replay & r, double estimate) {
    const Allocation & al = r.plan.allocation;
    const double s = al.seconds_per_token;
    const double mib = 1024.0 * 1024.0;
    std::printf("%-22s %-15s %9.0f %7.0f%% %9.0f %9.0f %9.3f %8.2f %8.2f%s\n", what.c_str(),
                r.plan.streaming_declined ? "declined" : regime_name(r.plan.regime), (double) al.cache_bytes / mib,
                100.0 * al.cache_hit_credited, (double) al.ledger.held / mib, (double) al.ledger.holdable_cap / mib, s,
                s > 0.0 ? 1.0 / s : 0.0, estimate > 0.0 ? 1.0 / estimate : 0.0, r.caveats.empty() ? "" : "  *");
}

// Exit 0, or 4 when a replay with nothing changed is no longer the plan the record carries.
int replay_main(const Args & a) {
    if (a.file.empty()) return usage();
    std::string text;
    if (!read_text_file(a.file, text)) {
        std::fprintf(stderr, "bmoe-planlab: cannot read %s\n", a.file.c_str());
        return 1;
    }
    // A fact changed is its line again: the last value of a key wins.
    if (!text.empty() && text.back() != '\n') text += '\n';
    for (const std::string & kv : a.sets)
        text += kv + "\n";
    PlanRecord rec;
    std::string err;
    std::vector<std::string> unknown;
    if (!parse_plan_record(text, rec, err, &unknown)) {
        std::fprintf(stderr, "bmoe-planlab: %s: %s\n", a.file.c_str(), err.c_str());
        return 1;
    }
    for (const std::string & k : unknown)
        std::fprintf(stderr, "warning: '%s' is not a key of a plan record; ignored\n", k.c_str());

    std::vector<std::string> problems;
    RunConfig cfg = record_config(rec, problems);
    for (const std::string & p : problems)
        std::fprintf(stderr, "warning: recorded configuration: %s\n", p.c_str());
    PlanRequest req = rec.request;
    for (const std::string & key : a.seen) {
        const ParamDesc * d = find_param(key);
        std::string e;
        d->set(cfg, d->get(a.cfg), e);
        if (!req.is_pinned(key.c_str())) req.pinned.push_back(key);
    }
    const bool as_recorded = a.seen.empty() && a.sets.empty();

    std::printf("replay: %s (engine %s)%s%s\n", a.file.c_str(), rec.engine.c_str(), rec.note.empty() ? "" : ", ",
                rec.note.c_str());
    std::printf("replay: machine %s; model %s, %.1f GiB\n", rec.hw.label.c_str(), rec.model.arch.c_str(),
                (double) rec.model.file_bytes / (1024.0 * 1024.0 * 1024.0));

    if (!a.sweep.empty()) {
        const size_t eq = a.sweep.find('=');
        const ParamDesc * d = eq == std::string::npos ? nullptr : find_param(a.sweep.substr(0, eq));
        if (!d) {
            std::fprintf(stderr, "bmoe-planlab: --sweep wants PARAM=V1,V2,... with PARAM a parameter key\n");
            return 2;
        }
        std::printf("%-22s %-15s %9s %8s %9s %9s %9s %8s %8s\n", d->key.c_str(), "regime", "cache MiB", "hits",
                    "held MiB", "cap MiB", "s/token", "tok/s", "est t/s");
        const auto est = [&](const Replay & x) {
            return estimate_decode(x.plan, rec.hw, rec.model, placement_for(rec, x.plan)).seconds;
        };
        const Replay chosen = replay_plan(rec, cfg, req);
        print_row("(the plan's choice)", chosen, est(chosen));
        if (!req.is_pinned(d->key.c_str())) req.pinned.push_back(d->key);
        std::stringstream values(a.sweep.substr(eq + 1));
        std::string v;
        bool starred = false;
        while (std::getline(values, v, ',')) {
            RunConfig at = cfg;
            if (!d->set(at, v, err)) {
                std::fprintf(stderr, "bmoe-planlab: %s=%s: %s\n", d->key.c_str(), v.c_str(), err.c_str());
                return 2;
            }
            const Replay r = replay_plan(rec, at, req);
            starred = starred || !r.caveats.empty();
            print_row(v, r, est(r));
        }
        if (starred) std::printf("  * planned on less than the record holds; replay that value alone to read why\n");
        return 0;
    }

    const Replay r = replay_plan(rec, cfg, req);
    std::fputs(r.plan.explain().c_str(), stdout);
    std::printf("plan: reproduce with\n  bmoe-cli -m MODEL %s\n", r.plan.to_flags().c_str());
    for (const std::string & c : r.caveats)
        std::printf("replay: caveat: %s\n", c.c_str());
    const double s = r.plan.allocation.seconds_per_token;
    if (s > 0.0) std::printf("replay: predicts %.3f s/token (%.2f tok/s)\n", s, 1.0 / s);
    print_estimate("replay", estimate_decode(r.plan, rec.hw, rec.model, placement_for(rec, r.plan)));
    if (rec.ran()) {
        const Facts & o = rec.observed;
        const double measured = fact(o, "seconds_per_token");
        std::printf("replay: the recorded run measured %.3f s/token (%.2f tok/s) over %.0f tokens, cache hits %.1f%% "
                    "in decode; it had been predicted %.3f\n",
                    measured, measured > 0.0 ? 1.0 / measured : 0.0, fact(o, "generated_tokens"),
                    fact(o, "cache_hit_decode_pct", -1.0), fact(rec.predicted, "seconds_per_token"));
        // The cache the run had beside the cache the prediction was priced at.
        if (fact(o, "cache_budget_mib") > 0.0)
            std::printf("replay: that run's cache: %.0f MiB resident of a %.0f MiB budget, %.0f experts dropped by "
                        "the system; this plan credits hits at %.0f MiB\n",
                        fact(o, "cache_resident_mib"), fact(o, "cache_budget_mib"), fact(o, "cache_discarded"),
                        (double) r.plan.allocation.cache_bytes / (1024.0 * 1024.0));
    }
    const std::string now = resolved_line(r.plan);
    int rc = 0;
    if (as_recorded) {
        if (now == rec.resolved) {
            std::printf("replay: SAME plan as recorded\n");
        } else {
            std::printf("replay: DIFFERENT plan from the one recorded\n  recorded: %s\n  now:      %s\n",
                        rec.resolved.c_str(), now.c_str());
            rc = 4;
        }
    }
    // Written back, the record carries this build's plan: how a recorded plan is accepted as
    // changed, and how a what-if (a pinned knob, a changed fact) becomes a record of its own.
    if (!a.out.empty()) {
        PlanRecord fresh = make_plan_record(cfg, req, rec.hw, rec.model, rec.placements, r.plan);
        fresh.note = rec.note;
        fresh.env = rec.env;
        if (as_recorded) fresh.observed = rec.observed; // a what-if was never run
        if (!write_text_file(a.out, format_plan_record(fresh))) {
            std::fprintf(stderr, "bmoe-planlab: cannot write %s\n", a.out.c_str());
            return 1;
        }
        std::printf("replay: record written to %s\n", a.out.c_str());
        rc = 0;
    }
    return rc;
}

// ── simulate ────────────────────────────────────────────────────────────────────────────────
int simulate_main(const Args & a) {
    if (a.file.empty() || a.cfg.model_path.empty() || a.out.empty()) return usage();
    std::string text, err;
    if (!read_text_file(a.file, text)) {
        std::fprintf(stderr, "bmoe-planlab: cannot read %s\n", a.file.c_str());
        return 1;
    }
    MachineSpec spec;
    const std::string dir = std::filesystem::u8path(a.file).parent_path().u8string();
    if (!parse_machine_spec(text, dir, spec, err)) {
        std::fprintf(stderr, "bmoe-planlab: %s: %s\n", a.file.c_str(), err.c_str());
        return 1;
    }
    PlanRequest req;
    for (const std::string & key : a.seen)
        req.pinned.push_back(key);
    const Simulation sim = simulate_machine(spec, a.cfg.model_path, a.cfg, req);
    if (!sim.error.empty()) {
        std::fprintf(stderr, "bmoe-planlab: %s\n", sim.error.c_str());
        return 1;
    }
    std::printf("simulate: %s\n", sim.record.note.c_str());
    std::fputs(sim.plan.explain().c_str(), stdout);
    std::printf("plan: reproduce with\n  bmoe-cli -m MODEL %s\n", sim.plan.to_flags().c_str());
    for (const std::string & c : sim.caveats)
        std::printf("simulate: caveat: %s\n", c.c_str());
    const double s = sim.plan.allocation.seconds_per_token;
    if (s > 0.0) std::printf("simulate: the cost model predicts %.3f s/token (%.2f tok/s)\n", s, 1.0 / s);
    print_estimate("simulate",
                   estimate_decode(sim.plan, sim.record.hw, sim.record.model, placement_for(sim.record, sim.plan)));
    if (!write_text_file(a.out, format_plan_record(sim.record))) {
        std::fprintf(stderr, "bmoe-planlab: cannot write %s\n", a.out.c_str());
        return 1;
    }
    std::printf("simulate: record written to %s\n", a.out.c_str());
    return 0;
}

// ── the records together ─────────────────────────────────────────────────────────────────────
std::vector<std::string> records_in(const std::string & dir) {
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto & e : std::filesystem::recursive_directory_iterator(std::filesystem::u8path(dir), ec))
        if (e.path().extension() == ".planrec") files.push_back(e.path().u8string());
    std::sort(files.begin(), files.end());
    return files;
}

std::string csv_cell(const std::string & v) {
    if (v.find_first_of(",\"\n") == std::string::npos) return v;
    std::string out = "\"";
    for (char c : v)
        out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

// Every record under DIR as one table: a row per record, a column per key any of them carries. The
// keys are the record's own, so the table is whatever was measured - the facts the plan was made
// from, what it predicted in parts, what the run measured - with nothing chosen here.
int table_main(const Args & a) {
    if (a.file.empty()) return usage();
    std::vector<std::map<std::string, std::string>> rows;
    std::set<std::string> columns;
    for (const std::string & path : records_in(a.file)) {
        std::string text, line;
        if (!read_text_file(path, text)) continue;
        std::map<std::string, std::string> row;
        row["file"] = std::filesystem::u8path(path).filename().u8string();
        std::istringstream in(text);
        std::getline(in, line); // the header
        while (std::getline(in, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos || line[0] == '#') continue;
            row[line.substr(0, eq)] = line.substr(eq + 1);
        }
        for (const auto & e : row)
            columns.insert(e.first);
        rows.push_back(std::move(row));
    }
    std::string out;
    for (const std::string & c : columns)
        out += (out.empty() ? "" : ",") + csv_cell(c);
    out += "\n";
    for (const auto & row : rows) {
        bool first = true;
        for (const std::string & c : columns) {
            const auto it = row.find(c);
            out += (first ? "" : ",") + (it == row.end() ? std::string() : csv_cell(it->second));
            first = false;
        }
        out += "\n";
    }
    if (a.out.empty()) {
        std::fputs(out.c_str(), stdout);
    } else if (!write_text_file(a.out, out)) {
        std::fprintf(stderr, "bmoe-planlab: cannot write %s\n", a.out.c_str());
        return 1;
    }
    std::fprintf(stderr, "planlab: %zu records, %zu columns\n", rows.size(), columns.size());
    return 0;
}

// Every record under DIR that carries a run: what THIS build's rules predict for it beside what the
// run measured, the total and the two parts a plan prices (computing the token, reading experts),
// and the cache it credited beside the cache the run had. The standing list of where the cost model
// is wrong, and by how much, on every machine on record.
int errors_main(const Args & a) {
    if (a.file.empty()) return usage();
    std::printf("%-52s %-8s %7s %8s %8s %7s | %8s %8s | %8s %8s | %7s %7s\n", "record", "os", "RAM GiB", "pred s/t",
                "meas s/t", "error", "pred hit", "meas hit", "cache MiB", "had MiB", "swap MiB", "majflt/t");
    int n = 0;
    double sum_abs = 0.0;
    for (const std::string & path : records_in(a.file)) {
        std::string text, err;
        PlanRecord rec;
        if (!read_text_file(path, text) || !parse_plan_record(text, rec, err) || !rec.ran()) continue;
        std::vector<std::string> problems;
        const Replay r = replay_plan(rec, record_config(rec, problems), rec.request);
        const Facts & o = rec.observed;
        // The settled half of the decode where the record has it: a plan predicts the steady state.
        const double measured = fact(o, "token.wall_ms.settled_p50") > 0.0 ? fact(o, "token.wall_ms.settled_p50") / 1e3
                                                                           : fact(o, "seconds_per_token");
        const double predicted = r.plan.allocation.seconds_per_token;
        if (measured <= 0.0 || predicted <= 0.0) continue;
        const double error = 100.0 * (predicted / measured - 1.0);
        ++n;
        sum_abs += error < 0 ? -error : error;
        const auto os = rec.env.find("os");
        std::printf("%-52.52s %-8.8s %7.1f %8.3f %8.3f %+6.0f%% | %7.0f%% %7.1f%% | %8.0f %8.0f | %7.0f %7.2f\n",
                    std::filesystem::u8path(path).filename().u8string().c_str(),
                    os == rec.env.end() ? "?" : os->second.c_str(),
                    (double) rec.hw.memory_total / (1024.0 * 1024.0 * 1024.0), predicted, measured, error,
                    100.0 * r.plan.allocation.cache_hit_credited, fact(o, "cache_hit_decode_pct", -1.0),
                    (double) r.plan.allocation.cache_bytes / (1024.0 * 1024.0), fact(o, "cache_resident_mib"),
                    fact(o, "memory.process_swap_mib.peak"), fact(o, "majflt_per_token"));
    }
    if (n)
        std::printf("%d record(s) with a run; mean absolute error of the predicted s/token %.0f%%\n", n, sum_abs / n);
    else
        std::printf("no record with a run under %s\n", a.file.c_str());
    return 0;
}

// ── parts: the model of parts, and how well it predicts a machine it was not fitted on ────────
int parts_main(const Args & a) {
    if (a.file.empty()) return usage();
    std::string text, err;
    if (!read_text_file(a.file, text)) {
        std::fprintf(stderr, "bmoe-planlab: cannot read %s\n", a.file.c_str());
        return 1;
    }
    PartsCatalog cat;
    if (!parse_parts_catalog(text, std::filesystem::u8path(a.file).parent_path().u8string(), cat, err)) {
        std::fprintf(stderr, "bmoe-planlab: %s: %s\n", a.file.c_str(), err.c_str());
        return 1;
    }
    PartsModel pm;
    if (!fit_parts_model(cat, pm, err)) {
        std::fprintf(stderr, "bmoe-planlab: %s\n", err.c_str());
        return 1;
    }
    auto line = [](const char * what, const Term & t, double scale, const char * unit) {
        std::printf("  %-44s %10.3f %-6s  measured/model %.2f..%.2f over %d\n", what, t.value * scale, unit, t.lo, t.hi,
                    t.n);
    };
    std::printf("fitted on %zu measured machines\n", pm.fitted_on.size());
    line("card experts: efficiency of spec bandwidth", pm.gpu_expert_eff, 1, "");
    line("card experts: latency per layer", pm.gpu_expert_lat_s, 1e6, "us");
    line("card dense: efficiency of spec bandwidth", pm.gpu_dense_eff, 1, "");
    line("card dense: latency per layer", pm.gpu_dense_lat_s, 1e6, "us");
    line("card probe bandwidth / spec", pm.gpu_probe_bw, 1, "");
    line("upload from host memory / link", pm.gpu_pinned, 1, "");
    line("upload from the mapping / link", pm.gpu_pageable, 1, "");
    line("graph split", pm.gpu_split_s, 1e6, "us");
    line("host plateau / spec memory bandwidth", pm.host_plateau_of_membw, 1, "");
    line("host parallel efficiency (compute-bound)", pm.host_parallel_eff, 1, "");
    line("host experts / plateau", pm.host_expert_of_plateau, 1, "");
    line("host dense / plateau", pm.host_dense_of_plateau, 1, "");
    for (const auto & [k, t] : pm.r1_per_ghz)
        line(("one thread per GHz: " + k).c_str(), t, 1, "GiB/s");
    std::printf("\nleave one out: each record's measured rates against the model fitted without its machine\n%s",
                check_parts_model(cat).c_str());
    return 0;
}

// ── speed: the decode estimate against every measured run ───────────────────────────────────
int speed_main(const Args & a) {
    if (a.file.empty()) return usage();
    std::printf("%-56s %9s %9s %7s  %s\n", "record", "est ms", "meas ms", "meas/est", "parts (ms)");
    std::vector<double> ratios;
    for (const std::string & path : records_in(a.file)) {
        std::string text, err;
        PlanRecord rec;
        if (!read_text_file(path, text) || !parse_plan_record(text, rec, err) || !rec.ran()) continue;
        std::vector<std::string> problems;
        const Replay r = replay_plan(rec, record_config(rec, problems), rec.request);
        const DecodeEstimate e = estimate_decode(r.plan, rec.hw, rec.model, placement_for(rec, r.plan));
        const Facts & o = rec.observed;
        const double measured = fact(o, "token.wall_ms.settled_p50") > 0.0 ? fact(o, "token.wall_ms.settled_p50") / 1e3
                                                                           : fact(o, "seconds_per_token");
        if (e.seconds <= 0.0 || measured <= 0.0) continue;
        ratios.push_back(measured / e.seconds);
        std::string parts;
        for (const auto & [name, sec] : e.parts) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s%s %.1f", parts.empty() ? "" : ", ", name.c_str(), 1e3 * sec);
            parts += buf;
        }
        std::printf("%-56.56s %9.1f %9.1f %7.2f  %s\n", std::filesystem::u8path(path).filename().u8string().c_str(),
                    1e3 * e.seconds, 1e3 * measured, measured / e.seconds, parts.c_str());
    }
    if (ratios.empty()) {
        std::printf("no record with a run and an estimate under %s\n", a.file.c_str());
        return 0;
    }
    std::sort(ratios.begin(), ratios.end());
    const auto q = [&](double p) { return ratios[(size_t) std::lround(p * (double) (ratios.size() - 1))]; };
    std::printf("%zu run(s): measured/estimated median %.2f, lowest %.2f, highest %.2f\n", ratios.size(), q(0.5),
                ratios.front(), ratios.back());
    return 0;
}

} // namespace

int main(int argc, char ** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    if (cmd != "record" && cmd != "replay" && cmd != "simulate" && cmd != "speed" && cmd != "parts" && cmd != "table" &&
        cmd != "errors")
        return usage();
    const Args a = parse_args(argc, argv, 2, cmd != "record");
    if (!a.ok) return 2;
    return cmd == "record"     ? record_main(a)
           : cmd == "replay"   ? replay_main(a)
           : cmd == "simulate" ? simulate_main(a)
           : cmd == "speed"    ? speed_main(a)
           : cmd == "parts"    ? parts_main(a)
           : cmd == "table"    ? table_main(a)
                               : errors_main(a);
}
