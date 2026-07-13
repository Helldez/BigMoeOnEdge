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

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
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
        case 'n': o += '\n'; break;
        case 'r': o += '\r'; break;
        case 't': o += '\t'; break;
        case '"': o += '"'; break;
        case '\\': o += '\\'; break;
        case '/': o += '/'; break;
        case 'u':
            if (i + 4 < s.size()) {
                int code = (int) std::strtol(s.substr(i + 1, 4).c_str(), nullptr, 16);
                // The protocol only carries ASCII control chars as \u00xx (from json_escape);
                // decode those directly. Anything else is passed through as the literal char.
                o += (char) (code & 0xff);
                i += 4;
            }
            break;
        default: o += c; break;
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
    while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
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
    while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
    return line.compare(p, 4, "true") == 0;
}

// A parsed stdin command. cancel is handled inline by the reader thread (it calls
// Session::cancel directly), so only generate/close travel through the queue.
struct SessionCmd {
    enum Kind { kGenerate, kClose } kind;
    std::string prompt;
    int id = 0;
    int n_predict = 32;
    bool think = true;
    bool clear_kv = true;
};

// Interactive session: keep the model loaded and the expert cache warm across prompts, reading
// one JSON request per line from stdin and emitting the BMOE_* line protocol on stdout. See
// docs/telemetry.md. Returns the process exit code.
static int run_session_loop(const RunConfig & cfg, IMetricsSink * sink) {
    SessionConfig sc;
    sc.model_path = cfg.model_path;
    sc.n_threads = cfg.n_threads;
    sc.n_ctx = cfg.n_ctx;
    sc.n_batch = cfg.n_ctx; // one-batch prefill for any prompt that fits the context
    sc.chatml = cfg.chatml;
    sc.moe = cfg.moe;

    std::string error;
    std::unique_ptr<Session> session = Session::open(sc, error);
    if (!session) {
        std::printf("BMOE_ERROR {\"id\":0,\"fatal\":true,\"msg\":\"%s\"}\n", json_escape(error).c_str());
        std::fflush(stdout);
        return 1;
    }
    std::printf("BMOE_READY {\"load_s\":%.3f,\"arch\":\"%s\",\"n_ctx\":%d}\n", session->load_seconds(),
                json_escape(session->arch()).c_str(), session->n_ctx());
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

        auto on_token = [&](const TokenMetrics & m) {
            if (m.read_bytes || m.io_ms > 0.0)
                std::printf("BMOE_LOAD {\"mb\":%.2f,\"ms\":%.1f}\n", m.read_bytes / (1024.0 * 1024.0), m.io_ms);
            std::printf("BMOE_PROGRESS {\"step\":%d,\"steps\":%d,\"wall_ms\":%.1f,\"io_ms\":%.1f,"
                        "\"compute_ms\":%.1f,\"cache_hit_pct\":%.1f,\"text\":\"%s\"}\n",
                        m.step, m.steps, m.wall_ms, m.io_ms, m.compute_ms, m.cache_hit_pct,
                        json_escape(m.text).c_str());
            std::fflush(stdout);
        };

        GenerateRequest req;
        req.prompt = cmd.prompt;
        req.n_predict = cmd.n_predict;
        req.think = cmd.think;
        req.clear_kv = cmd.clear_kv;

        RunResult r = session->generate(req, on_token, sink);
        if (!r) {
            // A bad request (empty prompt, context overflow) leaves the session usable; a decode
            // failure means the context is compromised, so end the loop.
            bool recoverable =
                r.error.find("exceeds the session n_ctx") != std::string::npos ||
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
                    "\"load_s\":%.3f,\"cache_hit_pct\":%.1f,\"text\":\"%s\"}\n",
                    cmd.id, r.cancelled ? "true" : "false", s.n_generated, s.tokens_per_second, s.prefill_seconds,
                    s.load_seconds, s.cache_hit_pct, json_escape(r.generated_text).c_str());
        std::fflush(stdout);
    }

    // Unblock the reader if it is still waiting on stdin (it exits on EOF; on an explicit close
    // it has usually already returned). Detach so process exit is not held up by a blocking read.
    if (reader.joinable()) reader.detach();
    return rc;
}

static void print_usage(const char * argv0) {
    std::printf("usage: %s -m <model.gguf> [options]\n"
                "\n"
                "  -m, --model PATH        gguf model (required)\n"
                "  -p, --prompt STR        prompt text\n"
                "  -n, --n-predict N       tokens to generate (default 32)\n"
                "  -t, --threads N         compute threads (default 4)\n"
                "  -c, --ctx-size N        context size (default 2048)\n"
                "      --chatml            wrap the prompt in the model family's chat turn (gemma/chatml)\n"
                "      --no-think          render the chat template with reasoning disabled\n"
                "      --progress          emit machine telemetry (one JSON line per token)\n"
                "      --session           keep the model loaded and serve JSON prompt requests from stdin\n"
                "      --csv PATH          also write per-token metrics as CSV\n"
                "\n"
                "  MoE expert streaming:\n"
                "      --moe-stream        stream only the routed experts per token (MoE models)\n"
                "      --cache-mb N        LRU expert cache budget in MiB (0=off, or >=%d)\n"
                "      --io-threads N      parallel expert-read lanes [1..%d] (default 4)\n"
                "      --no-odirect        do not bypass the page cache\n"
                "      --load-all          debug: read ALL experts each token (A/B baseline)\n"
                "      --force-cache       allow a cache-mb in the pathological band\n"
                "      --overlap           overlap async expert reads with FFN compute (needs the fork)\n"
                "      --list-archs        print supported MoE architectures and exit\n"
                "\n"
                "  Env overrides (flag wins): BMOE_CACHE_MB, BMOE_IO_THREADS, BMOE_PROGRESS, BMOE_OVERLAP\n",
                argv0, MoeStreamConfig::cache_min_mb, MoeStreamConfig::io_threads_max);
}

int main(int argc, char ** argv) {
    RunConfig cfg;
    std::string csv_path;
    bool session_mode = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
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
        else if (a == "--moe-stream")
            cfg.moe.enabled = true;
        else if (a == "--cache-mb")
            cfg.moe.cache_mb = std::atoi(next("--cache-mb"));
        else if (a == "--io-threads")
            cfg.moe.io_threads = std::atoi(next("--io-threads"));
        else if (a == "--no-odirect")
            cfg.moe.o_direct = false;
        else if (a == "--load-all")
            cfg.moe.load_all = true;
        else if (a == "--force-cache")
            cfg.moe.force_cache = true;
        else if (a == "--overlap")
            cfg.moe.overlap = true;
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

    // Env overrides (flag wins: only apply when the flag left the default).
    if (cfg.moe.cache_mb == 0) cfg.moe.cache_mb = env_int("BMOE_CACHE_MB", 0);
    if (cfg.moe.io_threads == 4) cfg.moe.io_threads = env_int("BMOE_IO_THREADS", 4);
    if (!cfg.progress) cfg.progress = env_int("BMOE_PROGRESS", 0) != 0;
    if (!cfg.moe.overlap) cfg.moe.overlap = env_int("BMOE_OVERLAP", 0) != 0;

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

    // Interactive session: one persistent process serves many prompts over stdin, keeping the
    // model loaded and the expert cache warm between them. Prompts arrive as JSON requests, not
    // via -p. This is a superset of --progress output (BMOE_* lines), so it never streams inline.
    if (session_mode) return run_session_loop(cfg, sink.get());

    if (!cfg.progress) {
        std::printf("%s", cfg.prompt.c_str());
        std::fflush(stdout);
    }

    auto on_token = [&](const TokenMetrics & m) {
        if (cfg.progress) {
            if (m.read_bytes || m.io_ms > 0.0)
                std::printf("BMOE_LOAD {\"mb\":%.2f,\"ms\":%.1f}\n", m.read_bytes / (1024.0 * 1024.0), m.io_ms);
            std::printf("BMOE_PROGRESS {\"step\":%d,\"steps\":%d,\"wall_ms\":%.1f,\"io_ms\":%.1f,"
                        "\"compute_ms\":%.1f,\"cache_hit_pct\":%.1f,\"text\":\"%s\"}\n",
                        m.step, m.steps, m.wall_ms, m.io_ms, m.compute_ms, m.cache_hit_pct,
                        json_escape(m.text).c_str());
            std::fflush(stdout);
        } else {
            std::fwrite(m.piece.data(), 1, m.piece.size(), stdout);
            std::fflush(stdout);
        }
    };

    RunResult r = run(cfg, on_token, sink.get());
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
        if (s.cache_hit_pct >= 0.0)
            std::printf("moe-cache: %.1f%% hit, resident %.1f MiB\n", s.cache_hit_pct, s.cache_resident_mib);
        if (cfg.moe.overlap)
            std::printf("moe-overlap: stall %.3f s/token (flash reads overlapped with FFN compute)\n",
                        s.moe_stall_s_per_token);
    }
    return 0;
}
