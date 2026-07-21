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
#include "bmoe/runtime.h"
#include "bmoe/session.h"
#include "bmoe/recipe.h"
#include "bmoe/metrics.h"
#include "bmoe/route_trace.h"
#include "bmoe/decode_trace.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

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

// One token's line-protocol output: the optional BMOE_LOAD, then BMOE_PROGRESS (docs/telemetry.md).
// Both emitters — the one-shot --progress run and the interactive session, which is a superset of it
// — must produce a byte-identical line, since the Android app parses one parser's worth of protocol.
// Keeping the format string in one place is what makes that true rather than merely intended.
static void emit_progress_line(const TokenMetrics & m) {
    if (m.read_bytes || m.io_ms > 0.0)
        std::printf("BMOE_LOAD {\"mb\":%.2f,\"ms\":%.1f}\n", m.read_bytes / (1024.0 * 1024.0), m.io_ms);
    std::printf("BMOE_PROGRESS {\"step\":%d,\"steps\":%d,\"wall_ms\":%.1f,\"io_ms\":%.1f,"
                "\"compute_ms\":%.1f,\"mgmt_ms\":%.1f,\"stall_ms\":%.1f,\"read_mb\":%.2f,"
                "\"cache_hit_pct\":%.1f,\"majflt\":%llu,\"cpu_ms\":%.1f,\"dense_resident_frac\":%.3f,"
                "\"reasoning\":\"%s\",\"text\":\"%s\"}\n",
                m.step, m.steps, m.wall_ms, m.io_ms, m.compute_ms, m.mgmt_ms, m.stall_ms,
                m.read_bytes / (1024.0 * 1024.0), m.cache_hit_pct, (unsigned long long) m.majflt, m.cpu_ms,
                m.dense_resident_frac, json_escape(m.reasoning).c_str(), json_escape(m.text).c_str());
    std::fflush(stdout);
}

// ── minimal flat-JSON reading for the --session request protocol ──
// The session request objects are flat (string/int/bool fields only), so a tiny hand-rolled
// extractor keeps the CLI dependency-free, mirroring the hand-written JSON it already emits.

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

static bool json_get_string(const std::string & line, const char * key, std::string & out) {
    size_t p = json_value_pos(line, key);
    if (p == std::string::npos) return false;
    while (p < line.size() && (line[p] == ' ' || line[p] == '\t'))
        ++p;
    if (p >= line.size() || line[p] != '"') return false;
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
    return true;
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
// Session::cancel directly), so only generate/close travel through the queue.
struct SessionCmd {
    enum Kind { kGenerate, kClose } kind;
    std::string prompt;
    int id = 0;
    int n_predict = 128;
    bool think = true;
    bool clear_kv = true;
};

// Interactive session: keep the model loaded and the expert cache warm across prompts, reading
// one JSON request per line from stdin and emitting the BMOE_* line protocol on stdout. See
// docs/telemetry.md. Returns the process exit code.
static int run_session_loop(const RunConfig & cfg, IMetricsSink * sink, IRouteTraceSink * route_trace) {
    const SessionConfig sc = session_config_from(cfg);

    std::string error;
    std::unique_ptr<Session> session = Session::open(sc, error, route_trace);
    if (!session) {
        std::printf("BMOE_ERROR {\"id\":0,\"fatal\":true,\"msg\":\"%s\"}\n", json_escape(error).c_str());
        std::fflush(stdout);
        return 1;
    }
    // think_ctl states, once, whether this model can honour a think=false request at all, so a UI
    // can disable its Thinking control instead of leaving one that silently does nothing (#82).
    std::printf("BMOE_READY {\"load_s\":%.3f,\"arch\":\"%s\",\"n_ctx\":%d,\"think_ctl\":\"%s\"}\n",
                session->load_seconds(), json_escape(session->arch()).c_str(), session->n_ctx(),
                bmoe::think_control_name(session->think_control()));
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
            queue.push_back({SessionCmd::kClose, "", 0, 0, true, true});
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

        std::printf("BMOE_BEGIN {\"id\":%d}\n", cmd.id);
        std::fflush(stdout);

        GenerateRequest req;
        req.prompt = cmd.prompt;
        req.n_predict = cmd.n_predict;
        req.think = cmd.think;
        req.clear_kv = cmd.clear_kv;

        RunResult r = session->generate(req, emit_progress_line, sink);
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
                    "\"token_demand_mib\":%.1f,\"reasoning\":\"%s\",\"text\":\"%s\"}\n",
                    cmd.id, r.cancelled ? "true" : "false", s.n_generated, s.tokens_per_second, s.prefill_seconds,
                    (s.prefill_seconds > 0 ? s.n_prompt / s.prefill_seconds : 0.0), s.load_seconds, s.cache_hit_pct,
                    s.n_prompt, s.n_past, s.moe_compute_s_per_token, s.moe_io_s_per_token, s.cache_resident_mib,
                    s.cache_budget_mib, s.moe_read_mib, s.moe_stall_s_per_token, s.moe_mgmt_s_per_token,
                    s.majflt_per_token, s.cpu_s_per_token, s.token_demand_mib, json_escape(r.reasoning_text).c_str(),
                    json_escape(r.generated_text).c_str());
        std::fflush(stdout);
    }

    // Unblock the reader if it is still waiting on stdin (it exits on EOF; on an explicit close
    // it has usually already returned). Detach so process exit is not held up by a blocking read.
    if (reader.joinable()) reader.detach();
    return rc;
}

static void print_usage(const char * argv0) {
    std::printf(
        "usage: %s -m <model.gguf> [options]\n"
        "\n"
        "  -m, --model PATH        gguf model (required)\n"
        "  -p, --prompt STR        prompt text\n"
        "  -n, --n-predict N       tokens to generate (default 128)\n"
        "  -t, --threads N         compute threads (default 4)\n"
        "  -c, --ctx-size N        context size (default 2048)\n"
        "      --chatml            wrap the prompt in the model family's chat turn (gemma/chatml)\n"
        "      --no-think          render the chat template with reasoning disabled\n"
        "      --progress          emit machine telemetry (one JSON line per token)\n"
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
        "      --n-expert-used N   override active MoE experts per token (top-k); lower = faster\n"
        "                          but changes the output (quality). 0 = model default\n"
        "\n"
        "  Sampling (default: greedy/argmax, deterministic):\n"
        "      --temp F            sampling temperature; <= 0 keeps greedy (default 0). > 0 enables\n"
        "                          the chain top-k -> top-p -> temp -> dist\n"
        "      --top-k N           top-k cutoff when sampling (0 disables the stage; default 40)\n"
        "      --top-p F           nucleus cutoff in (0,1] when sampling (default 0.95)\n"
        "      --seed N            RNG seed for sampling (default: random per run)\n"
        "\n"
        "  MoE expert streaming:\n"
        "      --moe-stream        stream only the routed experts per token (MoE models)\n"
        "      --cache-mb N|auto   LRU expert cache budget in MiB (0=off, or >=%d); auto=size to device\n"
        "      --cache-floor-mb N  with --cache-mb auto: RAM to leave free (default 1536)\n"
        "      --cache-ceil-mb N   with --cache-mb auto: upper bound on the budget (0 = no cap)\n"
        "      --io-threads N      parallel expert-read lanes [1..%d] (default 4)\n"
        "      --no-odirect        do not bypass the page cache for expert reads\n"
        "      --dense-weights M   dense (non-expert) weight policy: mmap | warm | anon (default) | ahwb\n"
        "                          (warm = page-cache them at load, best when the model fits in RAM;\n"
        "                          anon = read via O_DIRECT into our own buffers and rebind, so a\n"
        "                          reclaim hits zram not flash — the win on >RAM models;\n"
        "                          ahwb = as anon, but into dma-buf memory the kernel may not reclaim\n"
        "                          at all — not even to zram, which is what anon still pays for.\n"
        "                          Android-only; measured +17.9% on a long generation, off by default)\n"
        "      --load-all          debug: read ALL experts each token (A/B baseline)\n"
        "      --force-cache       allow a cache-mb in the pathological band\n"
        "      --overlap           overlap async expert reads with FFN compute (needs the fork)\n"
        "      --prefetch K        temporally prefetch the next K layers' experts (needs the cache)\n"
        "      --list-archs        print supported MoE architectures and exit\n"
        "\n"
        "  Env overrides (flag wins): BMOE_CACHE_MB, BMOE_IO_THREADS, BMOE_PROGRESS, BMOE_OVERLAP, BMOE_PREFETCH, "
        "BMOE_N_EXPERT_USED\n",
        argv0, MoeStreamConfig::cache_min_mb, MoeStreamConfig::io_threads_max);
}

int main(int argc, char ** argv) {
    RunConfig cfg;
    std::string csv_path;
    std::string route_trace_path;
    std::string compute_trace_path;
    std::string io_trace_path;
    bool session_mode = false;

    // Which flags the user actually typed. The env overrides below consult this rather than
    // comparing against the default, so passing a flag its default value still wins.
    std::set<std::string> seen;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        seen.insert(a);
        auto next = [&](const char * what) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "-m" || a == "--model")
            cfg.model_path = next("-m");
        else if (a == "-p" || a == "--prompt")
            cfg.prompt = next("-p");
        else if (a == "-n" || a == "--n-predict")
            cfg.n_predict = std::atoi(next("-n"));
        else if (a == "-t" || a == "--threads")
            cfg.n_threads = std::atoi(next("-t"));
        else if (a == "-c" || a == "--ctx-size")
            cfg.n_ctx = std::atoi(next("-c"));
        else if (a == "--n-expert-used")
            cfg.n_expert_used = std::atoi(next("--n-expert-used"));
        else if (a == "--temp")
            cfg.sampling.temp = (float) std::atof(next("--temp"));
        else if (a == "--top-k")
            cfg.sampling.top_k = std::atoi(next("--top-k"));
        else if (a == "--top-p")
            cfg.sampling.top_p = (float) std::atof(next("--top-p"));
        else if (a == "--seed")
            cfg.sampling.seed = (uint32_t) std::strtoul(next("--seed"), nullptr, 10);
        else if (a == "--chatml")
            cfg.chatml = true;
        else if (a == "--no-think")
            cfg.think = false;
        else if (a == "--progress")
            cfg.progress = true;
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
        else if (a == "--moe-stream")
            cfg.moe.enabled = true;
        else if (a == "--cache-mb") {
            const std::string v = next("--cache-mb");
            if (v == "auto")
                cfg.moe.cache_auto = true;
            else
                cfg.moe.cache_mb = std::atoi(v.c_str());
        } else if (a == "--cache-floor-mb")
            cfg.moe.cache_floor_mb = std::atoi(next("--cache-floor-mb"));
        else if (a == "--cache-ceil-mb")
            cfg.moe.cache_ceil_mb = std::atoi(next("--cache-ceil-mb"));
        else if (a == "--io-threads")
            cfg.moe.io_threads = std::atoi(next("--io-threads"));
        else if (a == "--no-odirect")
            cfg.moe.o_direct = false;
        else if (a == "--dense-weights") {
            const std::string m = next("--dense-weights");
            if (m == "mmap")
                cfg.moe.dense_weights = bmoe::DenseWeightsMode::Mmap;
            else if (m == "warm")
                cfg.moe.dense_weights = bmoe::DenseWeightsMode::Warmed;
            else if (m == "anon")
                cfg.moe.dense_weights = bmoe::DenseWeightsMode::Anonymous;
            else if (m == "ahwb")
                cfg.moe.dense_weights = bmoe::DenseWeightsMode::Pinned;
            else {
                std::fprintf(stderr, "bmoe: --dense-weights expects mmap|warm|anon|ahwb, got '%s'\n", m.c_str());
                return 2;
            }
        }
        // Deprecated aliases, kept so existing scripts and the app keep working: --no-warm-dense is
        // the Mmap policy, --dense-odirect is Anonymous. Prefer --dense-weights.
        else if (a == "--no-warm-dense")
            cfg.moe.dense_weights = bmoe::DenseWeightsMode::Mmap;
        else if (a == "--dense-odirect")
            cfg.moe.dense_weights = bmoe::DenseWeightsMode::Anonymous;
        else if (a == "--load-all")
            cfg.moe.load_all = true;
        else if (a == "--force-cache")
            cfg.moe.force_cache = true;
        else if (a == "--overlap")
            cfg.moe.overlap = true;
        else if (a == "--prefetch")
            cfg.moe.prefetch_layers = std::atoi(next("--prefetch"));
        else if (a == "--prefetch-sync") // debug: complete speculative reads synchronously
            cfg.moe.prefetch_sync = true;
        else if (a == "--list-archs") {
            std::printf("supported MoE architectures:\n");
            for (int k = 0; k < n_moe_recipes(); ++k)
                std::printf("  %s\n", moe_recipe_at(k)->arch);
            return 0;
        } else if (a == "-h" || a == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            print_usage(argv[0]);
            return 1;
        }
    }

    // Env overrides (flag wins: only apply when the flag was not passed). Asking whether the flag
    // was typed, not whether its value still equals the default, is what makes an explicit
    // --cache-mb 0 (cache off) or --io-threads 4 stick. The defaults below match config.h, so an
    // unset variable leaves the field alone.
    if (!seen.count("--cache-mb")) cfg.moe.cache_mb = env_int("BMOE_CACHE_MB", 0);
    if (!seen.count("--io-threads")) cfg.moe.io_threads = env_int("BMOE_IO_THREADS", 4);
    if (!seen.count("--progress")) cfg.progress = env_int("BMOE_PROGRESS", 0) != 0;
    if (!seen.count("--overlap")) cfg.moe.overlap = env_int("BMOE_OVERLAP", 0) != 0;
    if (!seen.count("--prefetch")) cfg.moe.prefetch_layers = env_int("BMOE_PREFETCH", 0);
    if (!seen.count("--n-expert-used")) cfg.n_expert_used = env_int("BMOE_N_EXPERT_USED", 0);

    if (cfg.model_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    ValidationResult vr = validate(cfg);
    if (!vr) {
        std::fprintf(stderr, "config error: %s\n", vr.error.c_str());
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
    if (session_mode) return run_session_loop(cfg, sink.get(), route_trace.get());

    if (!cfg.progress) {
        std::printf("%s", cfg.prompt.c_str());
        std::fflush(stdout);
    }

    auto on_token = [&](const TokenMetrics & m) {
        if (cfg.progress) {
            emit_progress_line(m);
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
    // Compute decomposition (0 s/tok CPU means the platform couldn't measure it — Windows host).
    // occupancy = CPU-time ÷ (wall × threads): ~1 is compute-bound, well under 1 is a throttled or
    // preempted core; major faults/token > 0 means dense weights re-faulted from flash inside decode.
    if (s.cpu_s_per_token > 0.0 || s.majflt_per_token > 0.0) {
        const double occ =
            s.s_per_token > 0 && cfg.n_threads > 0 ? s.cpu_s_per_token / (s.s_per_token * cfg.n_threads) : 0.0;
        std::printf("compute: %.1f%% CPU occupancy (%.4f cpu-s/token over %d threads), %.2f major faults/token\n",
                    occ * 100.0, s.cpu_s_per_token, cfg.n_threads, s.majflt_per_token);
    }
    if (s.n_prompt > 0) {
        double prefill_tps = s.prefill_seconds > 0 ? s.n_prompt / s.prefill_seconds : 0.0;
        std::printf("prefill: %d tokens, %.3f s (%.1f tok/s) | model load %.3f s | TTFT %.3f s\n", s.n_prompt,
                    s.prefill_seconds, prefill_tps, s.load_seconds, s.load_seconds + s.prefill_seconds);
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
        }
        if (cfg.moe.overlap)
            std::printf("moe-overlap: stall %.3f s/token (flash reads overlapped with FFN compute)\n",
                        s.moe_stall_s_per_token);
        if (cfg.moe.prefetch_layers > 0)
            std::printf("moe-prefetch: %.1f MiB speculative, %lld/%lld experts useful (%.0f%%)\n", s.moe_spec_read_mib,
                        s.moe_spec_useful, s.moe_spec_experts,
                        s.moe_spec_experts > 0 ? 100.0 * s.moe_spec_useful / s.moe_spec_experts : 0.0);
    }
    return 0;
}
