#include "warmup.h"

#include "engine_host.h"
#include "json_util.h"
#include "openai_chat.h"
#include "platform.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <utility>

using namespace bmoe;

// ~300 tokens' worth: small enough that a request arriving mid-replay waits for one segment and
// not the whole prefix. The value is from the fork's measured serve campaign, not tuned since.
namespace bmoe::server {

namespace {
constexpr size_t k_segment_chars = 1200;
// A completion boundary for the segment's render. Every segment ends in the same dummy, so the
// next (real or replayed) render diverges exactly there — the dummy is cut by the reuse diff and
// never becomes part of a real prefix.
const char * k_dummy = "(warmup)";

size_t content_chars(const json & m) {
    return m.contains("content") && m["content"].is_string() ? m["content"].get<std::string>().size() : 0;
}

json messages_json(const ChatRequest & turn, size_t keep) {
    json msgs = json::array();
    for (size_t i = 0; i < keep && i < turn.messages.size(); ++i) {
        const ChatMessage & m = turn.messages[i];
        json o = {{"role", m.role}, {"content", m.content}};
        if (!m.reasoning.empty()) o["reasoning_content"] = m.reasoning;
        msgs.push_back(std::move(o));
    }
    return msgs;
}

} // namespace

Warmup::Warmup(std::string file_path, bool enabled)
    : file_path_(std::move(file_path)), enabled_(enabled) {}

Warmup::~Warmup() {
    client_seen_.store(true);
    if (worker_.joinable()) worker_.join(); // at most one segment: a segment prefill is short by design
    for (std::thread & t : retired_)
        if (t.joinable()) t.join();
}

void Warmup::client_seen() { client_seen_.store(true); }

void Warmup::record(const std::string & model_path, const ChatRequest & turn) {
    if (!enabled_ || model_path.empty() || turn.n_predict <= 0) return;
    // A tool-calling conversation is not recorded; see the header for why a replay of one could
    // never match the real render closely enough to pay for itself.
    for (const ChatMessage & m : turn.messages)
        if (m.role == "tool" || !m.tool_calls.empty()) return;
    if (turn.messages.size() < 2) return;
    const size_t keep = turn.messages.size() - 1;
    if (keep == 0 || turn.messages[keep - 1].content.empty()) return; // nothing stable to warm

    std::lock_guard<std::mutex> lk(record_m_);
    std::filesystem::path path = std::filesystem::u8path(file_path_);
    const std::filesystem::path tmp(path.u8string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return; // an unwritable data directory turns warm-up off silently
        out << json{{"model", model_path}, {"messages", messages_json(turn, keep)}}.dump();
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (!ec) platform::restrict_file_to_user(file_path_);
}

void Warmup::restart(EngineHost & host, const RunConfig & cfg) {
    if (!enabled_ || cfg.model_path.empty()) return;
    std::lock_guard<std::mutex> lk(worker_m_);
    client_seen_.store(false); // a fresh sweep for the fresh model; the old one's sweep stops at its next boundary
    const int epoch = epoch_.fetch_add(1) + 1;
    // The current replay's thread object joins `retired_`, not removal: the loader must never wait
    // behind the turn line the old thread may hold. The old sweep exits at its next boundary.
    if (worker_.joinable()) retired_.push_back(std::move(worker_));
    worker_ = std::thread([this, &host, cfg, epoch] { run(&host, cfg, epoch); });
}

void Warmup::run(EngineHost * host, RunConfig cfg, int epoch) {
    json d;
    {
        std::ifstream in(std::filesystem::u8path(file_path_));
        if (!in) return;
        try {
            in >> d;
        } catch (const std::exception &) {
            return; // a corrupt or foreign file: warm-up is a best-effort feature, never an error
        }
    }
    const json msgs = d.value("messages", json());
    if (!msgs.is_array() || msgs.empty() || d.value("model", "") != cfg.model_path) return;

    // The load that asked for this replay may not have finished; wait, but give up the moment
    // this epoch is superseded or a client arrived while the model was loading.
    while (!client_seen_.load() && !host->ready()) {
        if (epoch_.load() != epoch) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (client_seen_.load() || epoch_.load() != epoch) return;

    std::fprintf(stderr, "bmoe-server: warm-up: replaying %zu prefix messages in segments\n", msgs.size());
    size_t i = 0;
    int seg = 0;
    while (i < msgs.size()) {
        if (client_seen_.load() || epoch_.load() != epoch) {
            if (seg > 0) std::fprintf(stderr, "bmoe-server: warm-up: superseded after %d segments\n", seg);
            return;
        }
        // One segment = whole messages up to ~k_segment_chars, plus the dummy user turn so the
        // render has a completion boundary. The engine's reuse rule cuts the next render at the
        // dummy, so everything before it stays resident across segments and into the next turn.
        size_t j = i, chars = 0;
        while (j < msgs.size() && (j == i || chars < k_segment_chars)) {
            chars += content_chars(msgs[j]);
            ++j;
        }
        json seg_msgs = json::array();
        for (size_t k = 0; k < j && k < msgs.size(); ++k) seg_msgs.push_back(msgs[k]);
        seg_msgs.push_back(json{{"role", "user"}, {"content", k_dummy}});

        OpenAiChatRequest w;
        std::string werr;
        if (!parse_chat_request(json{{"messages", seg_msgs}}, cfg, w, werr))
            return; // the stored shape does not parse under this build: warm-up skips quietly
        w.turn.n_predict = 0; // prefill only: the replay conditions the KV, it answers nothing
        const ChatOutcome out = host->chat(w.turn, nullptr, nullptr);
        if (out.status != 200) return; // the model was replaced or the turn failed: stop, stay quiet
        i = j;
        ++seg;
    }
    std::fprintf(stderr, "bmoe-server: warm-up complete: prefix resident (%d segments)\n", seg);
}

} // namespace bmoe::server
