// bmoe-planlab: the planner's bench (docs/plan-records.md). A developer's instrument, built with
// -DBMOE_BUILD_TOOLS=ON and carried by no release.
//
//   bmoe-planlab record -m MODEL -o FILE [--note TEXT] [--routing-dir DIR] [--run PROMPT] [flags]
//       On the machine: probe it and the model exactly as `bmoe-cli --auto` does, plan, and write
//       everything the plan was made from to FILE. With --run, also run the plan on PROMPT and add
//       what the run measured.
//
//   bmoe-planlab replay FILE [--set KEY=VALUE]... [--sweep PARAM=V1,V2,...] [-o FILE] [flags]
//       Anywhere: plan again from FILE, with no model, no device and no drive.
//
// `flags` are engine parameters, spelled as bmoe-cli spells them (the parameter table); each one
// typed is pinned, there as here.

#include "plan_record.h"

#include "bmoe/auto_plan.h"
#include "bmoe/measured_knobs.h"
#include "bmoe/params.h"
#include "bmoe/probe.h"
#include "bmoe/routing.h"
#include "bmoe/runtime.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
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

// ── record ──────────────────────────────────────────────────────────────────────────────────
int record_main(const Args & a) {
    RunConfig cfg = a.cfg;
    if (cfg.model_path.empty() || a.out.empty()) return usage();

    // The child of an isolated device probe (bmoe/auto_plan.h): one device, its facts, nothing else.
    if (!a.probe_device.empty()) {
        std::string line;
        if (!probe_device_child(cfg.model_path.c_str(), a.probe_device, line)) return 3;
        std::printf("%s\n", line.c_str());
        return 0;
    }

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
    PlanRecord rec = make_plan_record(cfg, req, planned.hw, planned.model, planned.placements, planned.plan);
    rec.note = a.note;
    std::fputs(planned.plan.explain().c_str(), stdout);

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

    // The plan's own run, for the prediction to stand beside.
    cfg = planned.plan.config;
    cfg.prompt = a.prompt;
    const RunResult r = run(cfg);
    if (!r) {
        std::fprintf(stderr, "bmoe-planlab: the run failed: %s\n", r.error.c_str());
        return 1;
    }
    const RunSummary & s = r.summary;
    rec.observed.generated_tokens = s.n_generated;
    rec.observed.seconds_per_token = s.s_per_token;
    rec.observed.compute_s_per_token = s.moe_compute_s_per_token;
    rec.observed.io_s_per_token = s.moe_io_s_per_token;
    rec.observed.cache_hit_decode_pct = s.cache_hit_decode_pct;
    rec.observed.prompt_tokens = s.n_prompt;
    rec.observed.prefill_seconds = s.prefill_seconds;
    if (!write_text_file(a.out, format_plan_record(rec))) return 1;
    std::printf("planlab: predicted %.3f s/token, measured %.3f over %d tokens\n", rec.predicted_s_per_token,
                s.s_per_token, s.n_generated);
    return 0;
}

// ── replay ──────────────────────────────────────────────────────────────────────────────────
// One line of a replay: what the plan holds and what it predicts, for a table of several.
void print_row(const std::string & what, const Replay & r) {
    const Allocation & al = r.plan.allocation;
    const double s = al.seconds_per_token;
    const double mib = 1024.0 * 1024.0;
    std::printf("%-22s %-15s %9.0f %7.0f%% %9.0f %9.0f %9.3f %8.2f%s\n", what.c_str(),
                r.plan.streaming_declined ? "declined" : regime_name(r.plan.regime), (double) al.cache_bytes / mib,
                100.0 * al.cache_hit_credited, (double) al.ledger.held / mib, (double) al.ledger.holdable_cap / mib, s,
                s > 0.0 ? 1.0 / s : 0.0, r.caveats.empty() ? "" : "  *");
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
        std::printf("%-22s %-15s %9s %8s %9s %9s %9s %8s\n", d->key.c_str(), "regime", "cache MiB", "hits", "held MiB",
                    "cap MiB", "s/token", "tok/s");
        print_row("(the plan's choice)", replay_plan(rec, cfg, req));
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
            print_row(v, r);
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
    if (rec.observed.any())
        std::printf("replay: the recorded run measured %.3f s/token (%.2f tok/s) over %d tokens, cache hits %.1f%% "
                    "in decode; it had been predicted %.3f\n",
                    rec.observed.seconds_per_token,
                    rec.observed.seconds_per_token > 0.0 ? 1.0 / rec.observed.seconds_per_token : 0.0,
                    rec.observed.generated_tokens, rec.observed.cache_hit_decode_pct, rec.predicted_s_per_token);

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

} // namespace

int main(int argc, char ** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    if (cmd != "record" && cmd != "replay") return usage();
    const Args a = parse_args(argc, argv, 2, cmd == "replay");
    if (!a.ok) return 2;
    return cmd == "record" ? record_main(a) : replay_main(a);
}
