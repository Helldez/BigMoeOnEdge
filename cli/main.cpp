// bmoe-cli — host driver for BigMoeOnEdge.
//
// Parses flags into a RunConfig and runs the engine. Two output modes:
//   * default: streams the generated text inline, per-token timing to stderr;
//   * --progress: one machine-readable JSON line per token (docs/telemetry.md), which
//     the Android example app parses for its live panel.
//
// Environment variables are read ONLY here, as overrides for the matching flags, so the
// engine stays env-free. The flag always wins over the env value.
#include "bmoe/config.h"
#include "bmoe/params.h"
#include "bmoe/planner.h"
#include "bmoe/probe.h"
#include "bmoe/runtime.h"
#include "bmoe/session.h"
#include "bmoe/recipe.h"
#include "bmoe/metrics.h"
#include "bmoe/route_trace.h"
#include "bmoe/decode_trace.h"
#include "bmoe/version.h"

#include "llama.h"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace bmoe;

static int env_int(const char * k, int dflt) {
    const char * v = std::getenv(k);
    return (v && *v) ? std::atoi(v) : dflt;
}

static std::string json_escape(const std::string & s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':
            o += "\\\"";
            break;
        case '\\':
            o += "\\\\";
            break;
        case '\n':
            o += "\\n";
            break;
        case '\r':
            o += "\\r";
            break;
        case '\t':
            o += "\\t";
            break;
        default:
            if ((unsigned char) c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", c);
                o += b;
            } else
                o += c;
        }
    }
    return o;
}

// What BMOE_PROGRESS already delivered this generation, so each line carries only the new tail.
// One state per generation: reset it (fresh object) when a new one begins.
struct ProgressDelta {
    std::string reasoning;
    std::string text;
};

static bool is_extension(const std::string & full, const std::string & prev) {
    return full.size() >= prev.size() && full.compare(0, prev.size(), prev) == 0;
}

// One token's line-protocol output: the optional BMOE_LOAD, then BMOE_PROGRESS (docs/telemetry.md).
// Both emitters — the one-shot --progress run and the interactive session, which is a superset of it
// — must produce a byte-identical line, since the Android app parses one parser's worth of protocol.
// Keeping the format string in one place is what makes that true rather than merely intended.
//
// The answer travels as a DELTA: sending the cumulative text every token made a generation of n
// tokens write, escape and parse O(n^2) bytes (#119). The common case appends the suffix since the
// last line; when a closing tag makes common_chat_parse retroactively reclassify answer text as
// reasoning, an append cannot express it, so the line carries the full snapshot with "reset":1 and
// the reader replaces instead of appending. The first line of a generation is a plain extension of
// the empty state. The full final text still travels in BMOE_DONE.
static void emit_progress_line(const TokenMetrics & m, ProgressDelta & st) {
    if (m.read_bytes || m.io_ms > 0.0)
        std::printf("BMOE_LOAD {\"mb\":%.2f,\"ms\":%.1f}\n", m.read_bytes / (1024.0 * 1024.0), m.io_ms);
    const bool ext = is_extension(m.reasoning, st.reasoning) && is_extension(m.text, st.text);
    const std::string d_reason = ext ? m.reasoning.substr(st.reasoning.size()) : m.reasoning;
    const std::string d_text = ext ? m.text.substr(st.text.size()) : m.text;
    std::printf("BMOE_PROGRESS {\"step\":%d,\"steps\":%d,\"wall_ms\":%.1f,\"io_ms\":%.1f,"
                "\"compute_ms\":%.1f,\"mgmt_ms\":%.1f,\"stall_ms\":%.1f,\"read_mb\":%.2f,"
                "\"cache_hit_pct\":%.1f,\"majflt\":%llu,\"cpu_ms\":%.1f,\"dense_resident_frac\":%.3f,"
                "%s\"delta_reasoning\":\"%s\",\"delta_text\":\"%s\"}\n",
                m.step, m.steps, m.wall_ms, m.io_ms, m.compute_ms, m.mgmt_ms, m.stall_ms,
                m.read_bytes / (1024.0 * 1024.0), m.cache_hit_pct, (unsigned long long) m.majflt, m.cpu_ms,
                m.dense_resident_frac, ext ? "" : "\"reset\":1,", json_escape(d_reason).c_str(),
                json_escape(d_text).c_str());
    st.reasoning = m.reasoning;
    st.text = m.text;
    std::fflush(stdout);
}

// ── minimal flat-JSON reading for the --session request protocol ──
// The session request objects are flat (string/int/bool fields, and decide's one array of
// strings), so a tiny hand-rolled extractor keeps the CLI dependency-free, mirroring the hand-written JSON it already
// emits.

static std::string json_unescape(const std::string & s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            o += s[i];
            continue;
        }
        char c = s[++i];
        switch (c) {
        case 'n':
            o += '\n';
            break;
        case 'r':
            o += '\r';
            break;
        case 't':
            o += '\t';
            break;
        case '"':
            o += '"';
            break;
        case '\\':
            o += '\\';
            break;
        case '/':
            o += '/';
            break;
        case 'u':
            if (i + 4 < s.size()) {
                int code = (int) std::strtol(s.substr(i + 1, 4).c_str(), nullptr, 16);
                // The protocol only carries ASCII control chars as \u00xx (from json_escape);
                // decode those directly. Anything else is passed through as the literal char.
                o += (char) (code & 0xff);
                i += 4;
            }
            break;
        default:
            o += c;
            break;
        }
    }
    return o;
}

// Find `"key"`, skip to its value. Returns the index just past the colon, or npos.
static size_t json_value_pos(const std::string & line, const char * key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t k = line.find(pat);
    if (k == std::string::npos) return std::string::npos;
    size_t c = line.find(':', k + pat.size());
    if (c == std::string::npos) return std::string::npos;
    return c + 1;
}

// Read the JSON string that starts at `p` (at or before its opening quote, after any blanks).
// Returns the index just past its closing quote, or npos when there is no string there.
static size_t json_read_string(const std::string & line, size_t p, std::string & out) {
    while (p < line.size() && (line[p] == ' ' || line[p] == '\t'))
        ++p;
    if (p >= line.size() || line[p] != '"') return std::string::npos;
    ++p;
    std::string raw;
    for (; p < line.size(); ++p) {
        if (line[p] == '\\' && p + 1 < line.size()) {
            raw += line[p];
            raw += line[p + 1];
            ++p;
        } else if (line[p] == '"') {
            break;
        } else {
            raw += line[p];
        }
    }
    out = json_unescape(raw);
    return p < line.size() ? p + 1 : std::string::npos;
}

static bool json_get_string(const std::string & line, const char * key, std::string & out) {
    size_t p = json_value_pos(line, key);
    if (p == std::string::npos) return false;
    return json_read_string(line, p, out) != std::string::npos;
}

// An array of strings, e.g. "choices":["A","B"]. False unless every element is a string.
static bool json_get_string_array(const std::string & line, const char * key, std::vector<std::string> & out) {
    size_t p = json_value_pos(line, key);
    if (p == std::string::npos) return false;
    while (p < line.size() && (line[p] == ' ' || line[p] == '\t'))
        ++p;
    if (p >= line.size() || line[p] != '[') return false;
    ++p;
    out.clear();
    for (;;) {
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t' || line[p] == ','))
            ++p;
        if (p >= line.size()) return false;
        if (line[p] == ']') return true;
        std::string item;
        p = json_read_string(line, p, item);
        if (p == std::string::npos) return false;
        out.push_back(std::move(item));
    }
}

static int json_get_int(const std::string & line, const char * key, int dflt) {
    size_t p = json_value_pos(line, key);
    if (p == std::string::npos) return dflt;
    return std::atoi(line.c_str() + p);
}

static bool json_get_bool(const std::string & line, const char * key, bool dflt) {
    size_t p = json_value_pos(line, key);
    if (p == std::string::npos) return dflt;
    while (p < line.size() && (line[p] == ' ' || line[p] == '\t'))
        ++p;
    return line.compare(p, 4, "true") == 0;
}

// A parsed stdin command. cancel is handled inline by the reader thread (it calls
// Session::cancel directly), so only generate/decide/close travel through the queue.
struct SessionCmd {
    enum Kind { kGenerate, kDecide, kClose } kind;
    std::string prompt;
    int id = 0;
    int n_predict = 128;
    bool think = true;
    bool clear_kv = true;
    // decide only (bmoe/decide.h)
    std::string prefix, suffix;
    std::vector<std::string> choices;
    bool reuse_prefix = true;
};

// Answer one decide request with a BMOE_DECIDE line, or a BMOE_ERROR one. Returns false when the
// session cannot go on (DecideResult::fatal).
static bool emit_decide(Session & session, const SessionCmd & cmd) {
    DecideRequest req;
    req.prefix = cmd.prefix;
    req.suffix = cmd.suffix;
    req.choices = cmd.choices;
    req.reuse_prefix = cmd.reuse_prefix;
    const DecideResult r = session.decide(req);
    if (!r.ok && !r.cancelled) {
        std::printf("BMOE_ERROR {\"id\":%d,\"fatal\":%s,\"msg\":\"%s\"}\n", cmd.id, r.fatal ? "true" : "false",
                    json_escape(r.error).c_str());
        std::fflush(stdout);
        return !r.fatal;
    }
    std::string logp = "[";
    for (size_t i = 0; i < r.choice_logp.size(); ++i) {
        char buf[32];
        // A choice the model gives no mass to is -inf, which JSON cannot carry: send null.
        if (std::isfinite(r.choice_logp[i]))
            std::snprintf(buf, sizeof buf, "%s%.6f", i ? "," : "", r.choice_logp[i]);
        else
            std::snprintf(buf, sizeof buf, "%snull", i ? "," : "");
        logp += buf;
    }
    logp += "]";
    const PrefillStats & p = r.prefill;
    std::printf("BMOE_DECIDE {\"id\":%d,\"cancelled\":%s,\"best\":%d,\"choice_logp\":%s,\"n_tokens\":%d,"
                "\"n_reused\":%d,\"n_prefilled\":%d,\"restore_s\":%.3f,\"store_s\":%.3f,\"prefill_s\":%.3f,"
                "\"prefill_cpu_s\":%.3f,\"prefill_read_mib\":%.1f,\"prefill_io_s\":%.3f,\"prefill_stall_s\":%.3f,"
                "\"prefill_mgmt_s\":%.3f,\"prefill_dev_tokens\":%d,\"prefill_dev_read_mib\":%.1f,"
                "\"prefill_dev_stall_s\":%.3f,\"prefill_dev_routed\":%lld,\"prefill_dev_demand\":%lld,"
                "\"prefix_state_mib\":%.1f}\n",
                cmd.id, r.cancelled ? "true" : "false", r.best, logp.c_str(), r.n_tokens, r.n_reused, r.n_prefilled,
                r.restore_seconds, r.store_seconds, p.seconds, p.cpu_seconds, p.read_mib, p.io_seconds, p.stall_seconds,
                p.mgmt_seconds, p.device_tokens, p.device_read_mib, p.device_stall_seconds, p.device_routed,
                p.device_demand, (double) r.prefix_state_bytes / (1024.0 * 1024.0));
    std::fflush(stdout);
    return true;
}

// Interactive session: keep the model loaded and the expert cache warm across prompts, reading
// one JSON request per line from stdin and emitting the BMOE_* line protocol on stdout. See
// docs/telemetry.md. Returns the process exit code.
static int run_session_loop(const RunConfig & cfg,
                            IMetricsSink * sink,
                            IRouteTraceSink * route_trace,
                            IComputeTraceSink * compute_trace,
                            IIoTraceSink * io_trace) {
    const SessionConfig sc = session_config_from(cfg);

    std::string error;
    std::unique_ptr<Session> session = Session::open(sc, error, route_trace, compute_trace, io_trace);
    if (!session) {
        std::printf("BMOE_ERROR {\"id\":0,\"fatal\":true,\"msg\":\"%s\"}\n", json_escape(error).c_str());
        std::fflush(stdout);
        return 1;
    }
    // think_ctl states, once, whether this model can honour a think=false request at all, so a UI
    // can disable its Thinking control instead of leaving one that silently does nothing (#82).
    // n_expert_used is the EFFECTIVE routing width, after any override. A UI needs it to say
    // anything sensible about --drop-cold-experts, whose threshold is a fraction of 1/top-k: the
    // same percentage trims a tail at 8 and takes half the routing at 2. 0 on a non-MoE model.
    std::printf("BMOE_READY {\"load_s\":%.3f,\"arch\":\"%s\",\"n_ctx\":%d,\"think_ctl\":\"%s\","
                "\"n_expert_used\":%d}\n",
                session->load_seconds(), json_escape(session->arch()).c_str(), session->n_ctx(),
                bmoe::think_control_name(session->think_control()), session->n_expert_used());
    std::fflush(stdout);

    std::mutex mtx;
    std::condition_variable cv;
    std::deque<SessionCmd> queue;
    std::atomic<bool> stop{false};

    // Reader thread: parse stdin lines. "cancel" is applied immediately (thread-safe) so it can
    // interrupt an in-flight generate; "generate"/"close" are queued for the main loop. EOF ends
    // the session like an explicit close.
    std::thread reader([&] {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::string cmd;
            if (!json_get_string(line, "cmd", cmd)) continue;
            if (cmd == "cancel") {
                session->cancel();
                continue;
            }
            SessionCmd c;
            if (cmd == "close") {
                c.kind = SessionCmd::kClose;
            } else if (cmd == "generate") {
                c.kind = SessionCmd::kGenerate;
                json_get_string(line, "prompt", c.prompt);
                c.id = json_get_int(line, "id", 0);
                c.n_predict = json_get_int(line, "n_predict", cfg.n_predict);
                c.think = json_get_bool(line, "think", cfg.think);
                c.clear_kv = json_get_bool(line, "clear_kv", true);
            } else if (cmd == "decide") {
                c.kind = SessionCmd::kDecide;
                c.id = json_get_int(line, "id", 0);
                json_get_string(line, "prefix", c.prefix);
                json_get_string(line, "suffix", c.suffix);
                json_get_string_array(line, "choices", c.choices);
                c.reuse_prefix = json_get_bool(line, "reuse_prefix", true);
            } else {
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(mtx);
                queue.push_back(std::move(c));
            }
            cv.notify_one();
        }
        {
            std::lock_guard<std::mutex> lk(mtx);
            stop.store(true);
            SessionCmd close;
            close.kind = SessionCmd::kClose;
            queue.push_back(std::move(close));
        }
        cv.notify_one();
    });

    int rc = 0;
    for (;;) {
        SessionCmd cmd;
        {
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait(lk, [&] { return !queue.empty(); });
            cmd = std::move(queue.front());
            queue.pop_front();
        }
        if (cmd.kind == SessionCmd::kClose) break;
        if (cmd.kind == SessionCmd::kDecide) {
            // Framed like a generation, so a front-end's busy state and wake lock need no special case.
            std::printf("BMOE_BEGIN {\"id\":%d}\n", cmd.id);
            std::fflush(stdout);
            if (!emit_decide(*session, cmd)) {
                rc = 1;
                break;
            }
            continue;
        }

        std::printf("BMOE_BEGIN {\"id\":%d}\n", cmd.id);
        std::fflush(stdout);

        GenerateRequest req;
        req.prompt = cmd.prompt;
        req.n_predict = cmd.n_predict;
        req.think = cmd.think;
        req.clear_kv = cmd.clear_kv;
        req.render_text = true; // the line protocol carries the parsed answer on every token

        ProgressDelta pd; // fresh per generation: the first line extends the empty state
        RunResult r = session->generate(req, [&](const TokenMetrics & m) { emit_progress_line(m, pd); }, sink);
        if (!r) {
            // A bad request (empty prompt, context overflow) leaves the session usable; a decode
            // failure means the context is compromised, so end the loop.
            bool recoverable = r.error.find("exceeds the session n_ctx") != std::string::npos ||
                               r.error.find("empty prompt") != std::string::npos;
            std::printf("BMOE_ERROR {\"id\":%d,\"fatal\":%s,\"msg\":\"%s\"}\n", cmd.id, recoverable ? "false" : "true",
                        json_escape(r.error).c_str());
            std::fflush(stdout);
            if (!recoverable) {
                rc = 1;
                break;
            }
            continue;
        }
        const RunSummary & s = r.summary;
        std::printf("BMOE_DONE {\"id\":%d,\"cancelled\":%s,\"tokens\":%d,\"tok_s\":%.3f,\"prefill_s\":%.3f,"
                    "\"prefill_tps\":%.2f,\"load_s\":%.3f,\"cache_hit_pct\":%.1f,\"n_prompt\":%d,\"n_past\":%d,"
                    "\"compute_s_tok\":%.4f,\"io_s_tok\":%.4f,\"cache_resident_mib\":%.0f,\"cache_budget_mib\":%.0f,"
                    "\"read_mib\":%.1f,\"stall_s_tok\":%.4f,\"mgmt_s_tok\":%.4f,\"majflt_tok\":%.2f,\"cpu_s_tok\":%.4f,"
                    "\"prefill_cpu_s\":%.3f,\"prefill_read_mib\":%.1f,\"prefill_io_s\":%.3f,"
                    "\"prefill_stall_s\":%.3f,\"prefill_mgmt_s\":%.3f,"
                    "\"prefill_dev_tokens\":%d,\"prefill_dev_nodes\":%lld,\"prefill_dev_read_mib\":%.1f,"
                    "\"prefill_dev_stall_s\":%.3f,"
                    "\"token_demand_mib\":%.1f,\"mtp_drafted\":%lld,\"mtp_accepted\":%lld,\"mtp_decodes\":%lld,"
                    "\"mtp_draft_s_tok\":%.4f,\"drafted_steps\":%lld,\"loop_overhead_s_tok\":%.4f,"
                    "\"reasoning\":\"%s\",\"text\":\"%s\"}\n",
                    cmd.id, r.cancelled ? "true" : "false", s.n_generated, s.tokens_per_second, s.prefill_seconds,
                    (s.prefill_seconds > 0 ? s.n_prompt / s.prefill_seconds : 0.0), s.load_seconds, s.cache_hit_pct,
                    s.n_prompt, s.n_past, s.moe_compute_s_per_token, s.moe_io_s_per_token, s.cache_resident_mib,
                    s.cache_budget_mib, s.moe_read_mib, s.moe_stall_s_per_token, s.moe_mgmt_s_per_token,
                    s.majflt_per_token, s.cpu_s_per_token, s.prefill_cpu_seconds, s.prefill_read_mib,
                    s.prefill_io_seconds, s.prefill_stall_seconds, s.prefill_mgmt_seconds, s.prefill_device_tokens,
                    s.prefill_device_nodes, s.prefill_device_read_mib, s.prefill_device_stall_seconds,
                    s.token_demand_mib, s.mtp_drafted, s.mtp_accepted, s.mtp_decodes, s.mtp_draft_s_per_token,
                    s.drafted_steps, s.loop_overhead_s_per_token, json_escape(r.reasoning_text).c_str(),
                    json_escape(r.generated_text).c_str());
        std::fflush(stdout);
    }

    // Unblock the reader if it is still waiting on stdin (it exits on EOF; on an explicit close
    // it has usually already returned). Detach so process exit is not held up by a blocking read.
    if (reader.joinable()) reader.detach();
    return rc;
}

// True when Explorer (a double click) created this console for us alone, so it will vanish the
// instant we return and nothing we printed gets read. A terminal the user already had open also
// holds the console and stays; the process count tells the two apart.
static bool console_is_ours_alone() {
#if defined(_WIN32)
    DWORD pid;
    return GetConsoleProcessList(&pid, 1) == 1;
#else
    return false;
#endif
}

// Print `text` word-wrapped to `width` columns, every line starting at column `indent`. The first
// line continues whatever the caller already printed on it, which is `first_col` columns wide.
static void print_wrapped(const std::string & text, size_t first_col, size_t indent, size_t width) {
    size_t col = first_col;
    if (col < indent) {
        std::printf("%*s", (int) (indent - col), "");
        col = indent;
    }
    bool line_empty = true;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && text[i] == ' ')
            ++i;
        size_t j = text.find(' ', i);
        if (j == std::string::npos) j = text.size();
        if (j == i) break;
        const size_t len = j - i;
        if (!line_empty && col + 1 + len > width) {
            std::printf("\n%*s", (int) indent, "");
            col = indent;
            line_empty = true;
        }
        if (!line_empty) {
            std::printf(" ");
            ++col;
        }
        std::printf("%.*s", (int) len, text.c_str() + i);
        col += len;
        line_empty = false;
        i = j;
    }
    std::printf("\n");
}

// The engine parameters' part of the usage text, generated from the parameter table so a knob added
// there is documented here without anyone remembering to.
static void print_param_usage() {
    const RunConfig defaults;
    const size_t indent = 26, width = 100;
    for (int g = 0; g <= (int) ParamGroup::Diagnostics; ++g) {
        bool header = false;
        for (const ParamDesc & d : params()) {
            if ((int) d.group != g) continue;
            if (!header) {
                std::printf("\n  %s:\n", param_group_label((ParamGroup) g));
                header = true;
            }
            std::string spell;
            if (!d.flag.empty()) {
                spell = d.short_flag.empty() ? "    " : d.short_flag + ", ";
                spell += d.flag + (d.value_hint.empty() ? "" : " " + d.value_hint);
            }
            for (const ParamSwitch & s : d.switches) {
                if (s.deprecated) continue;
                spell += (spell.empty() ? "    " : " | ") + s.flag;
            }
            std::string help = d.help;
            // The help describes the parameter, which for --no-odirect is the thing the flag turns
            // off; say so rather than print "Bypass the page cache" next to the flag that stops it.
            if (d.flag.empty() && d.type == ParamType::Bool && !d.switches.empty() && d.switches[0].value == "false")
                help = "Turns off (on by default): " + help;
            if (d.lossy) help = "LOSSY: " + help;
            if (d.level == ParamLevel::Experimental) help = "EXPERIMENTAL: " + help;
            if (d.level == ParamLevel::Debug) help = "debug: " + help;
            if (!d.flag.empty() && d.type != ParamType::Path) help += " (default " + d.get(defaults) + ")";
            for (const ParamSwitch & s : d.switches)
                if (s.deprecated) help += " Deprecated alias: " + s.flag + " = " + s.value + ".";

            std::printf("  %s", spell.c_str());
            const size_t col = 2 + spell.size();
            if (col + 2 > indent) {
                std::printf("\n");
                print_wrapped(help, 0, indent, width);
            } else {
                print_wrapped(help, col, indent, width);
            }
        }
    }
}

static void print_usage(const char * argv0) {
    std::printf("usage: %s -m <model.gguf> [options]\n"
                "\n"
                "  -p, --prompt STR        prompt text\n"
                "      --progress          emit machine telemetry (one JSON line per token)\n"
                "      --describe-params   print every engine parameter (key, type, bounds, default, help) as\n"
                "                          JSON and exit: the schema a front-end renders its settings from\n"
                "      --show-config       print the resolved configuration (flags, env overrides and CLI\n"
                "                          defaults applied) as JSON with its validation result, and exit\n"
                "      --auto              resolve the streaming knobs from what this machine and this model\n"
                "                          report (the hardware planner). A knob you also pass by hand is left\n"
                "                          exactly as you set it, and nothing lossy is ever armed\n"
                "      --plan              print the plan, the fact behind each choice and the flags that\n"
                "                          reproduce it, then exit without loading the model (= --plan-only)\n"
                "      --plan-explain      with --auto: print the plan, then run it\n"
                "      --probe             --plan, plus the memory probe below\n"
                "      --no-probe-io       plan without the storage read probe (touches no drive)\n"
                "      --no-probe-mem      never measure memory unasked. By default --auto measures it in one\n"
                "                          case only: a faster device was refused for lack of room, and the\n"
                "                          room was a reported figure rather than a measured one\n"
                "      --probe-mem         measure how much memory this machine will let us KEEP, by holding\n"
                "                          it until the kernel takes some back. The one probe that puts a live\n"
                "                          machine under real pressure, so it is off unless asked\n"
                "      --session           keep the model loaded and serve JSON prompt requests from stdin\n"
                "      --csv PATH          also write per-token metrics as CSV\n"
                "      --route-trace PATH  diagnostics: write the per-step per-layer MoE routing trace\n"
                "                          (which experts each layer routed, their weight, cache state).\n"
                "                          Needs --moe-stream; costs speed — not for benchmark runs\n"
                "      --compute-trace PATH\n"
                "                          diagnostics: isolate and time EVERY graph node (per-op detail,\n"
                "                          major faults per node). Serializes the graph — proportions only\n"
                "      --compute-trace-layers PATH\n"
                "                          same trace at layer granularity: one barrier per layer, so\n"
                "                          coalescing and the expert prefetch survive and the numbers stay\n"
                "                          close to an untraced run. Rows aggregate per layer (op LAYER)\n"
                "      --io-trace PATH     diagnostics: one row per expert read — its (layer, expert,\n"
                "                          projection), size and latency. Needs --moe-stream; this is how a\n"
                "                          flash-bandwidth claim is checked against the reads that made it\n"
                "  -h, --help              show this text and exit\n"
                "      --version           print the engine version and exit\n"
                "      --list-archs        print supported MoE architectures and exit\n"
                "\n"
                "  Perplexity (score a fixed text instead of generating):\n"
                "      --ppl FILE          measure teacher-forced perplexity of FILE instead of generating.\n"
                "                          Every cell scores the SAME fixed token sequence, so the number is\n"
                "                          a scale: comparing GENERATED text cannot price a lossy setting,\n"
                "                          because greedy output only moves when a perturbation happens to\n"
                "                          cross an argmax boundary, whatever its size\n"
                "      --ppl-skip N        leading tokens evaluated but not scored (default 8)\n"
                "      --ppl-step          score one token per decode, so a cache-dependent policy (dropping,\n"
                "                          substitution) is priced in the regime where it acts. A wide batch\n"
                "                          routes a layer before reading any of it, and finds almost nothing\n"
                "                          resident. Slower: one decode per token\n"
                "      --ppl-list FILE     score every text named in FILE (one path per line) in one session,\n"
                "                          so a benchmark of many short texts loads the model once\n"
                "      --ppl-choices A,B   after the text, report the log-probability of each choice's first\n"
                "                          token: the multiple-choice comparison, one pass per question\n",
                argv0);
    print_param_usage();
    std::printf("\n  With --moe-stream and no --cache-mb, the cache is sized automatically (--cache-mb auto).\n"
                "  Env overrides (flag wins): BMOE_CACHE_MB, BMOE_IO_THREADS, BMOE_PROGRESS, BMOE_OVERLAP, "
                "BMOE_PREFETCH,\n  BMOE_N_EXPERT_USED, BMOE_PREDICT_LOG, BMOE_PREDICT_PREFETCH\n");
}

// The prediction probe's report (see MoeStreamConfig::predict_log).
//
// Read the two predictors against the CONTROL rather than against 100%: the control ranks the same
// way with no staleness at all, so it is the ceiling this measurement can show on this model, and
// any gap below it is the ranking's approximation rather than the prediction's difficulty.
//
// The per-layer table is the substantive half. An aggregate flatters a prefetch: what a prefetch
// costs is set by the layers it gets wrong, and the first layers of a MoE model are reliably the
// worst — their routing scores sit close together, so a slightly stale input reorders them.
static void print_predict_report(const RunSummary & s) {
    const PredictorStats & st = s.predict_stale;
    const PredictorStats & pv = s.predict_prev;
    const PredictorStats & sf = s.predict_self;
    std::printf("moe-predict: stale-gate %.1f%% of routed slots (%.1f%% whole routings) | prev-token %.1f%% (%.1f%%)"
                " | fresh-gate control %.1f%% (%.1f%%)\n",
                100.0 * st.hit_frac(), 100.0 * st.exact_frac(), 100.0 * pv.hit_frac(), 100.0 * pv.exact_frac(),
                100.0 * sf.hit_frac(), 100.0 * sf.exact_frac());
    // The two-layer horizon, aggregate only: the staleness --predict-prefetch actually runs at.
    if (s.predict_stale2.rows > 0)
        std::printf("moe-predict: stale-2 (two layers early) %.1f%% of routed slots (%.1f%% whole routings)\n",
                    100.0 * s.predict_stale2.hit_frac(), 100.0 * s.predict_stale2.exact_frac());
    // Per predictor, because their denominators genuinely differ: the stale one cannot speak for
    // layer 0 (nothing precedes it) nor for the first token of a run, and quoting one row count for
    // all three would misread those structural gaps as agreement.
    std::printf("moe-predict: scored — stale-gate %lld routings/%lld slots, prev-token %lld/%lld, control %lld/%lld;"
                " %lld routings the stale-gate could not rank\n",
                st.rows, st.slots, pv.rows, pv.slots, sf.rows, sf.slots, s.predict_unscored);
    // Say it rather than let the reader assume staleness cost what the ranking did.
    if (sf.rows > 0 && sf.hit_frac() < 0.999)
        std::printf("moe-predict: the control is below 100%%, so this model's expert selection is not raw-logit\n"
                    "             ranking (an added selection bias, or group-limited routing). The stale-gate\n"
                    "             figure understates the method by about the control's own gap.\n");

    const size_t n = s.predict_stale_by_layer.size();
    if (n == 0) return;
    // A predictor with no routings at this layer prints "-", never 0.0. Layer 0 is the case that
    // matters: nothing precedes it, so the stale gate structurally cannot reach it — a limit of the
    // method, not a layer it predicts badly, and a table that showed 0.0% there would say the
    // opposite. (It is also the gap trained per-layer predictors exist to close.)
    auto pct = [](const PredictorStats & p, char * buf, size_t n_buf) -> const char * {
        if (p.rows == 0) {
            std::snprintf(buf, n_buf, "%6s", "-");
            return buf;
        }
        std::snprintf(buf, n_buf, "%6.1f", 100.0 * p.hit_frac());
        return buf;
    };
    std::printf("  layer   stale    prev   ctrl   routings\n");
    for (size_t il = 0; il < n; ++il) {
        const PredictorStats & a = s.predict_stale_by_layer[il];
        const PredictorStats b = il < s.predict_prev_by_layer.size() ? s.predict_prev_by_layer[il] : PredictorStats{};
        const PredictorStats c = il < s.predict_self_by_layer.size() ? s.predict_self_by_layer[il] : PredictorStats{};
        if (a.rows == 0 && b.rows == 0 && c.rows == 0) continue; // a dense layer routes nothing
        char ba[16], bb[16], bc[16];
        std::printf("  %5d  %s  %s %s     %6lld\n", (int) il, pct(a, ba, sizeof ba), pct(b, bb, sizeof bb),
                    pct(c, bc, sizeof bc), a.rows);
    }
}

int main(int argc, char ** argv) {
    RunConfig cfg;
    std::string csv_path;
    std::string route_trace_path;
    std::string ppl_path;
    int ppl_skip = 8;
    bool ppl_step = false;
    std::string ppl_list;                 // --ppl-list: one text path per line, scored in one session
    std::vector<std::string> ppl_choices; // --ppl-choices: strings whose first-token log-prob is reported
    std::string compute_trace_path;
    std::string io_trace_path;
    bool session_mode = false;

    bool describe_params = false;
    bool show_config = false;
    bool auto_plan = false;
    bool plan_explain = false;
    bool plan_only = false;
    bool probe_io = true;
    bool probe_mem = false; // off by default: it is the one probe that puts the machine under real pressure
    // ...and on by itself in exactly one case: a plan that says a decision is waiting on that figure
    // (Plan::headroom_wanted_bytes). Then it is bounded to what the decision needs.
    bool probe_mem_when_decisive = true;

    // Which parameters the user actually typed, by key. The env overrides below consult this rather
    // than comparing against the default, so passing a flag its default value still wins.
    std::set<std::string> seen;
    // Parameters whose switches are alternatives (--mtp / --ngram): the switch that set each one.
    std::map<std::string, std::string> switch_used;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char * what) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(1);
            }
            return argv[++i];
        };
        // Front-end flags first: outputs, modes and sinks, none of which is part of a run's
        // configuration. Everything else is an engine parameter and goes through the table.
        if (a == "-p" || a == "--prompt")
            cfg.prompt = next("-p");
        else if (a == "--progress") {
            cfg.progress = true;
            seen.insert("progress");
        } else if (a == "--describe-params")
            describe_params = true;
        else if (a == "--show-config")
            show_config = true;
        // Three commands over one machinery, because they answer three different questions: look at
        // the machine, say what you would do, do it. `--plan` prints the plan and the exact command
        // line that reproduces it, then exits - which is what makes a plan a VALUE that can be
        // pasted, diffed against a hand-tuned run and dropped into a bench cell.
        else if (a == "--auto" || a == "--plan-run")
            auto_plan = true;
        else if (a == "--plan" || a == "--plan-only") {
            auto_plan = plan_explain = plan_only = true;
        } else if (a == "--probe") {
            auto_plan = plan_explain = plan_only = true;
            probe_io = probe_mem = true;
        } else if (a == "--plan-explain")
            plan_explain = true;
        else if (a == "--probe-io") {
            auto_plan = true;
            probe_io = true;
        } else if (a == "--no-probe-io")
            probe_io = false;
        else if (a == "--probe-mem") {
            auto_plan = true;
            probe_mem = true;
        } else if (a == "--no-probe-mem")
            probe_mem_when_decisive = false;
        else if (a == "--session")
            session_mode = true;
        else if (a == "--csv")
            csv_path = next("--csv");
        else if (a == "--route-trace")
            route_trace_path = next("--route-trace");
        else if (a == "--compute-trace")
            compute_trace_path = next("--compute-trace");
        else if (a == "--compute-trace-layers") {
            compute_trace_path = next("--compute-trace-layers");
            cfg.compute_trace_layers = true;
        } else if (a == "--io-trace")
            io_trace_path = next("--io-trace");
        else if (a == "--ppl")
            ppl_path = next("--ppl");
        else if (a == "--ppl-skip")
            ppl_skip = std::atoi(next("--ppl-skip"));
        else if (a == "--ppl-step")
            ppl_step = true;
        else if (a == "--ppl-list")
            ppl_list = next("--ppl-list");
        else if (a == "--ppl-choices") {
            std::string cs = next("--ppl-choices");
            size_t start = 0;
            while (start <= cs.size()) {
                const size_t comma = cs.find(',', start);
                ppl_choices.push_back(cs.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else if (a == "--list-archs") {
            std::printf("supported MoE architectures:\n");
            for (int k = 0; k < n_moe_recipes(); ++k)
                std::printf("  %s\n", moe_recipe_at(k)->arch);
            return 0;
        } else if (a == "-h" || a == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (a == "--version") {
            std::printf("%s\n", bmoe::version());
            return 0;
        } else {
            const char * value = i + 1 < argc ? argv[i + 1] : nullptr;
            const FlagResult r = apply_flag(cfg, a.c_str(), value);
            if (!r.matched) {
                std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
                print_usage(argv[0]);
                return 1;
            }
            if (!r.error.empty()) {
                std::fprintf(stderr, "bmoe: %s\n", r.error.c_str());
                return value || r.via_switch ? 2 : 1;
            }
            if (r.consumed_value) ++i;
            seen.insert(r.param->key);
            // Two switches that are alternatives for one parameter are a contradiction rather than a
            // precedence question — say so instead of silently honouring the last one.
            if (r.via_switch && r.param->exclusive_switches) {
                auto it = switch_used.find(r.param->key);
                if (it != switch_used.end() && it->second != r.via_switch->flag) {
                    std::fprintf(stderr, "bmoe: %s and %s are alternatives (%s); choose one.\n", it->second.c_str(),
                                 r.via_switch->flag.c_str(), r.param->label.c_str());
                    return 2;
                }
                switch_used[r.param->key] = r.via_switch->flag;
            }
        }
    }

    if (describe_params) {
        std::printf("%s\n", params_json(RunConfig{}).c_str());
        return 0;
    }

    // Env overrides (flag wins: only apply when the parameter was not typed). Asking whether it was
    // typed, not whether its value still equals the default, is what makes an explicit --cache-mb 0
    // (cache off) or --io-threads 4 stick. An unset or empty variable leaves the field alone.
    struct EnvOverride {
        const char * var;
        const char * key;
    };
    static const EnvOverride kEnvOverrides[] = {
        {"BMOE_CACHE_MB", "cache-mb"},
        {"BMOE_IO_THREADS", "io-threads"},
        {"BMOE_OVERLAP", "overlap"},
        {"BMOE_PREFETCH", "prefetch"},
        {"BMOE_N_EXPERT_USED", "n-expert-used"},
        {"BMOE_PREDICT_LOG", "predict-log"},
        {"BMOE_PREDICT_PREFETCH", "predict-prefetch"},
    };
    for (const EnvOverride & e : kEnvOverrides) {
        const char * v = std::getenv(e.var);
        if (seen.count(e.key) || !v || !*v) continue;
        const ParamDesc * d = find_param(e.key);
        // Booleans keep the reading these variables always had: any non-zero integer is on.
        const std::string val = d->type == ParamType::Bool ? (std::atoi(v) != 0 ? "true" : "false") : v;
        std::string err;
        if (!d->set(cfg, val, err)) std::fprintf(stderr, "warning: ignoring %s: %s\n", e.var, err.c_str());
    }
    if (!seen.count("progress")) cfg.progress = env_int("BMOE_PROGRESS", 0) != 0;

    // A default the CLI resolves rather than the library, so an embedder's explicit 0 keeps meaning
    // "no cache". With streaming on, a budget of 0 re-reads every routed expert from flash every
    // token, which is never what someone who just typed --moe-stream wanted (#186). An explicit
    // --cache-mb or BMOE_CACHE_MB still wins, including an explicit 0.
    if (cfg.moe.enabled && !cfg.moe.cache_auto && !seen.count("cache-mb") && std::getenv("BMOE_CACHE_MB") == nullptr)
        cfg.moe.cache_auto = true;

    // --auto: resolve the streaming knobs from what the machine and the model report. It runs after
    // the flags, the env overrides and the CLI's own defaults, so that anything the caller expressed
    // either way is a pin the planner may not touch: an automatic choice that quietly overruled a
    // person would be worse than no automation at all. The pinned names are the parameter keys,
    // which is the name a Decision uses for its knob. See docs/hardware-planning.md.
    double predicted_s_per_token = 0.0;
    double predicted_hit_pct = -1.0;
    if (seen.count("dense-on-device")) auto_plan = true; // a request to the planner, as before
    if (auto_plan && !cfg.model_path.empty()) {
        PlanRequest req;
        req.pinned.assign(seen.begin(), seen.end());
        // An env override is the caller speaking too, so it pins the same way a flag does.
        for (const EnvOverride & e : kEnvOverrides)
            if (const char * v = std::getenv(e.var); v && *v) req.pinned.push_back(e.key);

        // QUIET FIRST, DIRTY LAST, and the order is a measurement rather than a preference: backends
        // registered before anything looks at devices; bandwidth and device costs before the storage
        // probe reads gigabytes; the intrusive headroom probe last of all, because it leaves the
        // kernel busy and anything measured after it measures the recovery.
        register_backends();
        HardwareProfile hw = probe_hardware(cfg.model_path.c_str());
        const ModelProfile mp = probe_model(cfg.model_path.c_str());
        probe_device_support(hw, mp);
        // The cache term is the FLOOR the engine will enforce, not one token cycle. Written without
        // std::max on purpose: windows.h defines `max` as a macro and this translation unit sees it.
        const uint64_t guard_bytes = (uint64_t) MoeStreamConfig::cache_min_mb << 20;
        const uint64_t cache_floor = mp.token_cycle_bytes > guard_bytes ? mp.token_cycle_bytes : guard_bytes;
        llama_backend_init();
        probe_bandwidth(hw, mp);
        probe_device_costs(hw, mp);
        if (probe_io) probe_storage(hw, cfg.model_path.c_str(), mp.expert_slice_bytes);
        probe_headroom(hw, probe_mem, mp.dense_bytes + cache_floor);
        const Placement placement = probe_placement(cfg.model_path.c_str(), mp, hw, (uint32_t) cfg.n_ctx);
        Plan plan = plan_run(cfg, hw, mp, placement, req);
        // The memory probe is intrusive, so it is spent only where it can change what the run does:
        // the plan refused something on memory alone, against a headroom it was told rather than
        // one it measured. Measure for exactly that much, and plan again. One round: a plan made
        // on a measured figure asks for nothing more.
        if (plan.headroom_wanted_bytes > 0 && probe_mem_when_decisive && !probe_mem) {
            const uint64_t before = hw.holdable_bytes ? hw.holdable_bytes : hw.residency_budget;
            std::fprintf(stderr,
                         "plan: a decision is waiting on memory: measuring whether %llu MiB can be held "
                         "(%llu reported; --no-probe-mem skips this)\n",
                         (unsigned long long) (plan.headroom_wanted_bytes >> 20), (unsigned long long) (before >> 20));
            probe_headroom(hw, true, plan.headroom_wanted_bytes);
            const uint64_t after = hw.holdable_bytes ? hw.holdable_bytes : hw.residency_budget;
            std::fprintf(stderr, "plan: %llu MiB %s\n", (unsigned long long) (after >> 20),
                         hw.holdable_from == Headroom::Measured ? "held and kept: planning again on the measured figure"
                                                                : "is still all that can be counted on");
            if (hw.holdable_from == Headroom::Measured) plan = plan_run(cfg, hw, mp, placement, req);
        }
        cfg = plan.config;
        predicted_s_per_token = plan.allocation.seconds_per_token;
        if (plan.allocation.cache_bytes > 0 && mp.expert_bytes > 0)
            predicted_hit_pct = 100.0 * (double) plan.allocation.cache_bytes / (double) mp.expert_bytes;

        if (plan_explain) {
            std::fprintf(stderr, "plan: machine %s\n", hw.label.c_str());
            // Every device with what it measured, so a decision about one can be checked against
            // the figures it was made from. 0 is "not measured", never "slow".
            for (const ComputeDevice & d : hw.devices) {
                const auto verdict = [](Tri t) {
                    return t == Tri::Yes ? "same result" : t == Tri::No ? "WRONG result" : "unverified";
                };
                std::fprintf(stderr, "plan: device %s%s: one token %.0f GiB/s (%s), %u tokens wide %.0f GiB/s (%s)\n",
                             d.name.c_str(), d.is_cpu ? " (host)" : "", d.memory_bandwidth_gibs, verdict(d.identity_ok),
                             hw.wide_batch, d.wide_matmul_gibs, verdict(d.wide_identity_ok));
            }
            if (mp.ok) {
                std::fprintf(stderr, "plan: model %s, %u experts top-%u over %u of %u blocks\n", mp.arch.c_str(),
                             mp.n_expert, mp.n_expert_used, mp.n_moe_layer, mp.n_layer);
                for (int gi = 0; gi < (int) WeightGroup::count; ++gi) {
                    const GroupDemand & d = mp.groups[gi];
                    if (d.bytes == 0) continue;
                    std::fprintf(stderr, "plan:   %-10s %7llu MiB, %7llu MiB/token%s\n", group_name((WeightGroup) gi),
                                 (unsigned long long) (d.bytes >> 20), (unsigned long long) (d.bytes_per_token >> 20),
                                 d.row_gatherable ? "  (row-gathered)"
                                 : d.streamable   ? "  (streamable)"
                                                  : "");
                }
            }
            std::fputs(plan.explain().c_str(), stderr);
            // The line that reproduces this plan by hand, printed last so it is what stays on screen.
            std::fprintf(stderr, "plan: reproduce with\n  bmoe-cli -m %s %s\n", cfg.model_path.c_str(),
                         plan.to_flags().c_str());
        }
        if (plan_only) return 0;
    }

    // The configuration exactly as a run would get it, and whether validate() accepts it. A
    // front-end reads this to check a form against the real resolution rules, not a copy of them.
    if (show_config) {
        const ValidationResult v = validate(cfg);
        std::string args;
        for (const std::string & s : to_args(cfg))
            args += (args.empty() ? "\"" : ",\"") + json_escape(s) + "\"";
        std::printf("{\"config\":%s,\"args\":[%s],\"valid\":%s,\"error\":\"%s\"}\n", config_json(cfg).c_str(),
                    args.c_str(), v.ok ? "true" : "false", json_escape(v.error).c_str());
        return v.ok ? 0 : 1;
    }

    if (cfg.model_path.empty()) {
        print_usage(argv[0]);
        // Double-clicked: without this the window closes before the usage can be read, and the
        // program looks like it failed to start.
        if (console_is_ours_alone()) {
            std::fprintf(stderr, "\nbmoe-cli is a command-line program: run it from a terminal with -m <model.gguf>.\n"
                                 "Press Enter to close this window.\n");
            std::getchar();
        }
        return 1;
    }

    ValidationResult vr = validate(cfg);
    if (!vr) {
        std::fprintf(stderr, "config error: %s\n", vr.error.c_str());
        return 1;
    }
    // Decide requests only travel through the session protocol; a one-shot run would ignore the flag.
    if (cfg.decide.enabled && !session_mode) {
        std::fprintf(stderr, "config error: --decide needs --session\n");
        return 1;
    }

    std::unique_ptr<IMetricsSink> sink;
    if (!csv_path.empty()) {
        sink.reset(make_csv_metrics_sink(csv_path));
        if (!sink) std::fprintf(stderr, "warning: could not open csv %s\n", csv_path.c_str());
    }

    std::unique_ptr<IRouteTraceSink> route_trace;
    if (!route_trace_path.empty()) {
        if (!cfg.moe.enabled) {
            // Say so rather than writing an empty file: without streaming there is no routing to
            // observe, and a header-only trace looks like a model that routed nothing.
            std::fprintf(stderr, "warning: --route-trace needs --moe-stream; no trace will be written\n");
        } else {
            route_trace.reset(make_csv_route_trace_sink(route_trace_path));
            if (!route_trace)
                std::fprintf(stderr, "warning: could not open route trace %s\n", route_trace_path.c_str());
        }
    }

    // The compute trace works without streaming — timing the graph is what a dense mmap baseline
    // needs too — so unlike the other two it carries no --moe-stream requirement.
    std::unique_ptr<IComputeTraceSink> compute_trace;
    if (!compute_trace_path.empty()) {
        compute_trace.reset(make_csv_compute_trace_sink(compute_trace_path));
        if (!compute_trace)
            std::fprintf(stderr, "warning: could not open compute trace %s\n", compute_trace_path.c_str());
    }

    std::unique_ptr<IIoTraceSink> io_trace;
    if (!io_trace_path.empty()) {
        if (!cfg.moe.enabled) {
            std::fprintf(stderr, "warning: --io-trace needs --moe-stream; no trace will be written\n");
        } else {
            io_trace.reset(make_csv_io_trace_sink(io_trace_path));
            if (!io_trace) std::fprintf(stderr, "warning: could not open io trace %s\n", io_trace_path.c_str());
        }
    }

    // Interactive session: one persistent process serves many prompts over stdin, keeping the
    // model loaded and the expert cache warm between them. Prompts arrive as JSON requests, not
    // via -p. This is a superset of --progress output (BMOE_* lines), so it never streams inline.
    if (session_mode) return run_session_loop(cfg, sink.get(), route_trace.get(), compute_trace.get(), io_trace.get());

    // Perplexity mode: score a fixed text instead of generating one. It opens the same session
    // with the same flags, so a lossy setting is priced under exactly the configuration it ships
    // with — and every cell scores the same tokens, which is the whole point.
    if (!ppl_path.empty() || !ppl_list.empty()) {
        std::vector<std::string> paths;
        if (!ppl_path.empty()) paths.push_back(ppl_path);
        if (!ppl_list.empty()) {
            std::ifstream lf(ppl_list);
            if (!lf) {
                std::fprintf(stderr, "bmoe: cannot open --ppl-list file '%s'\n", ppl_list.c_str());
                return 2;
            }
            std::string line;
            while (std::getline(lf, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (!line.empty() && line[0] != '#') paths.push_back(line);
            }
        }
        std::string error;
        const SessionConfig sc = session_config_from(cfg);
        std::unique_ptr<Session> session =
            Session::open(sc, error, route_trace.get(), compute_trace.get(), io_trace.get());
        if (!session) {
            std::fprintf(stderr, "bmoe: %s\n", error.c_str());
            return 1;
        }
        for (const std::string & path : paths) {
            std::ifstream tf(path, std::ios::binary);
            if (!tf) {
                std::fprintf(stderr, "bmoe: cannot open --ppl file '%s'\n", path.c_str());
                return 2;
            }
            std::ostringstream ts;
            ts << tf.rdbuf();
            PplRequest pr;
            pr.text = ts.str();
            pr.skip = ppl_skip;
            pr.step = ppl_step;
            pr.choices = ppl_choices;
            const PplResult pres = session->perplexity(pr);
            if (!pres.ok) {
                std::fprintf(stderr, "bmoe: perplexity failed on '%s': %s\n", path.c_str(), pres.error.c_str());
                return 1;
            }
            if (paths.size() > 1) std::printf("ppl-file: %s\n", path.c_str());
            std::printf("ppl: %.4f  nll: %.5f  next-token hits: %d/%d (%.1f%%)  %.2f s\n", pres.ppl, pres.nll,
                        pres.n_top1, pres.n_scored, pres.n_scored > 0 ? 100.0 * pres.n_top1 / pres.n_scored : 0.0,
                        pres.seconds);
            if (!pres.choice_logp.empty()) {
                std::printf("ppl-choices:");
                for (size_t k = 0; k < pres.choice_logp.size(); ++k)
                    std::printf(" %s=%.4f", ppl_choices[k].c_str(), pres.choice_logp[k]);
                std::printf("\n");
            }
            // Say what the policy did, always. A lossy flag that touched nothing scored the
            // baseline, and a table of identical perplexities is the least obvious way to be told so.
            std::printf("ppl-policy: %lld/%lld routed experts dropped, %lld/%lld reranked slots substituted\n",
                        pres.experts_dropped, pres.experts_routed, pres.experts_substituted, pres.experts_reranked);
            std::fflush(stdout);
        }
        return 0;
    }

    if (!cfg.progress) {
        std::printf("%s", cfg.prompt.c_str());
        std::fflush(stdout);
    }

    ProgressDelta pd; // one generation per one-shot run
    auto on_token = [&](const TokenMetrics & m) {
        if (cfg.progress) {
            emit_progress_line(m, pd);
        } else {
            std::fwrite(m.piece.data(), 1, m.piece.size(), stdout);
            std::fflush(stdout);
        }
    };

    RunResult r = run(cfg, on_token, sink.get(), route_trace.get(), compute_trace.get(), io_trace.get());
    if (!r) {
        std::fprintf(stderr, "\nerror: %s\n", r.error.c_str());
        return 1;
    }

    const RunSummary & s = r.summary;
    if (cfg.progress) {
        std::printf("=== answer ===\n%s\n=== perf ===\n", r.generated_text.c_str());
    } else {
        std::printf("\n\n");
    }
    std::printf("generation: %d tokens, %.3f s/token (%.3f tok/s)\n", s.n_generated, s.s_per_token,
                s.tokens_per_second);

    // The loop that closes. A plan predicted this run before it started; here is what it got. A cost
    // model whose error is never printed cannot be corrected, and the term the error is almost always
    // in is the credited cache hit rate, which is why the two hit figures sit next to each other.
    if (predicted_s_per_token > 0.0 && s.s_per_token > 0.0) {
        const double err = 100.0 * (predicted_s_per_token / s.s_per_token - 1.0);
        std::printf("plan: predicted %.3f s/token, measured %.3f (%+.0f%%)", predicted_s_per_token, s.s_per_token, err);
        if (predicted_hit_pct >= 0.0 && s.cache_hit_pct >= 0.0)
            std::printf("; cache hits credited %.0f%%, measured %.1f%%", predicted_hit_pct, s.cache_hit_pct);
        std::printf("\n");
    }
    // Compute decomposition (0 s/tok CPU means the platform couldn't measure it — Windows host).
    // occupancy = CPU-time ÷ (wall × threads): ~1 is compute-bound, well under 1 is a throttled or
    // preempted core; major faults/token > 0 means dense weights re-faulted from flash inside decode.
    if (s.cpu_s_per_token > 0.0 || s.majflt_per_token > 0.0) {
        const double occ =
            s.s_per_token > 0 && cfg.n_threads > 0 ? s.cpu_s_per_token / (s.s_per_token * cfg.n_threads) : 0.0;
        std::printf("compute: %.1f%% CPU occupancy (%.4f cpu-s/token over %d threads), %.2f major faults/token\n",
                    occ * 100.0, s.cpu_s_per_token, cfg.n_threads, s.majflt_per_token);
    }
    // Acceptance is the number that decides whether speculation can pay at all; tokens-per-decode is
    // what it actually bought, and it is what tok/s above is a function of.
    if (s.mtp_decodes > 0 && cfg.spec.enabled()) {
        const char * src = cfg.spec.is_mtp() ? "mtp" : "ngram";
        std::printf("%s: %lld/%lld drafts accepted (%.1f%%), %.2f tokens per verify decode "
                    "(%lld decodes for %d tokens)\n",
                    src, s.mtp_accepted, s.mtp_drafted,
                    s.mtp_drafted > 0 ? 100.0 * s.mtp_accepted / s.mtp_drafted : 0.0,
                    (double) s.n_generated / (double) s.mtp_decodes, s.mtp_decodes, s.n_generated);
        // tok/s above counts decode time only, so drafting is time the caller waits that the
        // headline rate does not show. Print what it actually costs, and the rate that includes it.
        const double eff =
            s.s_per_token + s.mtp_draft_s_per_token > 0 ? 1.0 / (s.s_per_token + s.mtp_draft_s_per_token) : 0.0;
        std::printf("%s: drafting costs %.4f s/token on top of decode → %.2f tok/s effective "
                    "(vs %.2f reported)\n",
                    src, s.mtp_draft_s_per_token, eff, s.tokens_per_second);
        // How often the source had anything to say. For the n-gram lookup this is the whole shape of
        // the result: the steps that did not draft ran at exactly the unspeculated cost, so a small
        // delta over baseline means something quite different at 10% coverage than at 90%.
        if (cfg.spec.is_ngram()) {
            std::printf("ngram: drafted on %lld of %lld steps (%.1f%%); the rest decoded plainly\n", s.drafted_steps,
                        s.mtp_decodes, s.mtp_decodes > 0 ? 100.0 * (double) s.drafted_steps / s.mtp_decodes : 0.0);
        }
        // Splits the run's flash bytes into the head's own routing and everything else, which is
        // the widened verify union. The two are attacked in completely different ways, and the
        // route trace cannot tell them apart — it brackets the target decode only. Only the head has
        // a share: the n-gram source reads no weights at all, so its split is 0 by construction.
        if (cfg.spec.is_mtp() && s.moe_read_mib > 0) {
            std::printf("mtp: of %.1f MiB streamed, %.1f MiB (%.1f%%) was the head's own routing, "
                        "%.1f MiB the widened verify batch\n",
                        s.moe_read_mib, s.mtp_draft_read_mib, 100.0 * s.mtp_draft_read_mib / s.moe_read_mib,
                        s.moe_read_mib - s.mtp_draft_read_mib);
        }
    }
    if (s.n_prompt > 0) {
        double prefill_tps = s.prefill_seconds > 0 ? s.n_prompt / s.prefill_seconds : 0.0;
        std::printf("prefill: %d tokens, %.3f s (%.1f tok/s) | model load %.3f s | TTFT %.3f s\n", s.n_prompt,
                    s.prefill_seconds, prefill_tps, s.load_seconds, s.load_seconds + s.prefill_seconds);
    }
    // What this run actually was. Printed unconditionally, because its absence was the defect: with
    // streaming off the engine is plain llama.cpp on mmap, every line below is silent, and a report
    // that only ever describes streaming let a baseline run read as a measurement of this project
    // (#186). Naming the flag here is cheaper than a doc nobody reaches from a terminal.
    {
        const std::string dense_name = find_param("dense-weights")->get(cfg);
        const char * dense = dense_name.c_str();
        if (cfg.moe.enabled) {
            char cache[64];
            if (cfg.moe.cache_auto)
                std::snprintf(cache, sizeof(cache), "cache auto");
            else if (cfg.moe.cache_mb > 0)
                std::snprintf(cache, sizeof(cache), "cache %d MiB", cfg.moe.cache_mb);
            else
                std::snprintf(cache, sizeof(cache), "cache off");
            std::printf("mode: expert streaming, %s, dense %s%s\n", cache, dense, cfg.moe.overlap ? ", overlap" : "");
        } else if (!s.arch.empty() && find_moe_recipe(s.arch.c_str())) {
            std::printf("mode: mmap. Expert streaming is OFF on a MoE model (%s), so this run is a "
                        "baseline, not this engine: add --moe-stream --overlap to stream the routed "
                        "experts from flash.\n",
                        s.arch.c_str());
        } else {
            std::printf("mode: mmap (%s is not a MoE architecture this build streams; --list-archs "
                        "lists the supported ones)\n",
                        s.arch.empty() ? "the model" : s.arch.c_str());
        }
    }
    if (cfg.moe.enabled) {
        std::printf("moe-stream: read %.1f MiB (%.2f MiB/token), decode %.3f s/token "
                    "(compute %.3f + cache mgmt %.3f + flash I/O %.3f s/token, %.0f MiB/s)\n",
                    s.moe_read_mib, s.n_generated ? s.moe_read_mib / s.n_generated : 0.0, s.s_per_token,
                    s.moe_compute_s_per_token, s.moe_mgmt_s_per_token, s.moe_io_s_per_token,
                    s.moe_io_seconds > 0 ? s.moe_read_mib / s.moe_io_seconds : 0.0);
        if (s.cache_hit_pct >= 0.0) {
            // The budget is worth printing only when the engine chose it: with an explicit --cache-mb
            // the reader already knows the number they passed.
            if (cfg.moe.cache_auto)
                std::printf("moe-cache: %.1f%% hit, resident %.1f MiB, budget %.0f MiB (auto)\n", s.cache_hit_pct,
                            s.cache_resident_mib, s.cache_budget_mib);
            else
                std::printf("moe-cache: %.1f%% hit, resident %.1f MiB\n", s.cache_hit_pct, s.cache_resident_mib);
            // Churn: a read of an entry the cache already held once. The bytes a routing needs are
            // fixed, so this is where any surplus goes — and the number to compare across an A/B
            // whose byte count moved.
            if (s.cache_evictions > 0 || s.cache_rereads > 0)
                std::printf("moe-cache: %lld evictions, %lld re-reads (%.1f/token) — bytes the cache had "
                            "already paid for once\n",
                            s.cache_evictions, s.cache_rereads,
                            s.n_generated ? (double) s.cache_rereads / s.n_generated : 0.0);
        }
        // The row policy's own line: what it took out of RAM, what it holds instead, and what
        // that cost in reads. Printed only when a table qualified, so a run that discovered
        // none says nothing rather than printing a row of zeroes.
        if (s.row_table_mib > 0.0) {
            std::printf("moe-rows: %.0f MiB of row-gathered table(s) off the resident set, %.1f MiB resident, "
                        "%lld rows gathered, %lld reads (%.1f MiB)\n",
                        s.row_table_mib, s.row_resident_mib, s.row_rows, s.row_slab_reads, s.row_read_mib);
            if (s.row_io_errors > 0)
                std::printf("moe-rows: %lld FAILED reads — this run's output is not trustworthy\n", s.row_io_errors);
        }
        if (cfg.moe.overlap)
            std::printf("moe-overlap: stall %.3f s/token (flash reads overlapped with FFN compute)\n",
                        s.moe_stall_s_per_token);
        // The named eval-thread waits, printed only when they cost something: the previous-batch
        // drain lives inside the compute residual, the adoption wait inside mgmt. Either being
        // large is a finding, not a footnote — both were invisible before they had meters.
        if (s.moe_drain_s_per_token >= 0.0005 || s.moe_adopt_s_per_token >= 0.0005)
            std::printf("moe-waits: drain %.3f s/token (inside compute), adopt %.3f s/token (inside mgmt)\n",
                        s.moe_drain_s_per_token, s.moe_adopt_s_per_token);
        if (cfg.moe.prefetch_layers > 0 || cfg.moe.predict_prefetch || cfg.moe.route_ahead > 0)
            std::printf("moe-prefetch: %.1f MiB speculative, %lld/%lld experts useful (%.0f%%)%s\n",
                        s.moe_spec_read_mib, s.moe_spec_useful, s.moe_spec_experts,
                        s.moe_spec_experts > 0 ? 100.0 * s.moe_spec_useful / s.moe_spec_experts : 0.0,
                        cfg.moe.route_ahead > 0    ? " [route-ahead]"
                        : cfg.moe.predict_prefetch ? " [stale-gate]"
                                                   : "");
        // How hard the policy actually bit. The flag sets a threshold, not a drop rate: what gets
        // discarded depends on what the cache held, so this is the only honest report of the trade
        // a given run made.
        if (cfg.moe.drop_cold_frac > 0.0f)
            std::printf("moe-drop: %lld/%lld routed experts dropped (%.1f%%), threshold %.2f x uniform\n",
                        s.experts_dropped, s.experts_routed,
                        s.experts_routed > 0 ? 100.0 * s.experts_dropped / s.experts_routed : 0.0,
                        (double) cfg.moe.drop_cold_frac);
        if (cfg.moe.substitute_lambda > 0.0f)
            std::printf("moe-substitute: %lld/%lld reranked slots went to a resident expert (%.1f%%), margin %.2f x "
                        "score range\n",
                        s.experts_substituted, s.experts_reranked,
                        s.experts_reranked > 0 ? 100.0 * s.experts_substituted / s.experts_reranked : 0.0,
                        (double) cfg.moe.substitute_lambda);
        // The agreement is the honest label for what the run just generated under: 100% minus it
        // is the fraction of routed slots that went to an expert the router did not choose.
        if (cfg.moe.route_ahead > 0) {
            std::printf("moe-route-ahead: %d layers early — %lld routings committed, %lld passed through; "
                        "committed selection agreed with the router on %.1f%% of slots\n",
                        cfg.moe.route_ahead, s.route_ahead_overridden, s.route_ahead_passthrough,
                        s.route_ahead_slots > 0 ? 100.0 * s.route_ahead_hits / s.route_ahead_slots : 0.0);
            // The prediction's own CPU, per token. On a host with spare cores this hides from the
            // wall clock; on a phone it competes with the decode, so it is printed either way.
            if (s.route_ahead_gemv_jobs > 0)
                std::printf("moe-route-ahead: prediction %.1f ms/token of worker CPU (%lld GEMVs, %.3f ms each)"
                            " + %.1f ms/token issuing the reads + %.1f ms/token watchdog, both on the eval thread\n",
                            s.n_generated ? s.route_ahead_gemv_ns / 1e6 / s.n_generated : 0.0, s.route_ahead_gemv_jobs,
                            s.route_ahead_gemv_ns / 1e6 / s.route_ahead_gemv_jobs,
                            s.n_generated ? s.route_ahead_issue_ns / 1e6 / s.n_generated : 0.0,
                            s.n_generated ? s.route_ahead_wd_ns / 1e6 / s.n_generated : 0.0);
        }
        if (cfg.moe.predict_log) print_predict_report(s);
    }
    return 0;
}
