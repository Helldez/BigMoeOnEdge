// Conversation warm-up for the server: the stable head of a chat conversation is saved (one file,
// keyed by model) after finished turns, and replayed into the KV as prefill-only segments the next
// time that model loads — so the first request of a fresh server pays only its own tokens.
//
// Why it is safe on any model: the replay only ever renders what a real client MIGHT resend. The
// session's own reuse rule diffs the next render against what the KV holds and cuts at the first
// divergence, so the worst case a changed prefix can cause is wasted prefill compute inside the
// segments that ran by then — never a wrong answer. The save excludes the last message on purpose:
// caching the message being answered would speculate on content edits and retries may replace,
// and that costs one message's prefill next start.
//
// Why segments: each segment releases the turn line when it returns, so a request that arrives
// mid-warm-up waits at most one segment; the remaining segments are skipped and the residency the
// finished segments built stays. The segment size is chars, not messages, so one huge file the
// client sent cannot stretch the wait.
#pragma once

#include "chat_turn.h"

#include "bmoe/config.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bmoe::server {

class EngineHost;

class Warmup {
public:
    Warmup(std::string file_path, bool enabled);

    // Not restartable and not copyable: it owns a thread.
    Warmup(const Warmup &) = delete;
    Warmup & operator=(const Warmup &) = delete;
    ~Warmup();

    // A real client turn (chat or decide) is starting: skip the remaining segments. What already
    // ran stays resident; the wait the client feeling this is bounded by one segment.
    void client_seen();

    // Record a conversation after a finished, non-cancelled generation turn. The in-flight user
    // message is dropped from what is stored. Tool-calling conversations are not recorded: their
    // history renders through the tool schemas of the turn that answers them, which the replay
    // does not carry, so a replayed prefix could never match the real render closely enough to
    // pay for itself.
    void record(const std::string & model_path, const ChatRequest & turn);

    // Replay the saved prefix of `cfg.model_path` once `host` is ready. Call this after each load
    // (the thread waits for Ready itself); it supersedes a replay still running for another model
    // at its next segment boundary.
    void restart(EngineHost & host, const RunConfig & cfg);

private:
    void run(EngineHost * host, RunConfig cfg, int epoch);

    const std::string file_path_;
    const bool enabled_;

    std::mutex record_m_; // record() runs on HTTP workers; the file write is short but not atomic

    // Client turns are seen from the HTTP handlers and read by the replay thread.
    std::atomic<bool> client_seen_{false};
    // Every restart raises this; a replay thread still running for the previous model reads a
    // stale epoch at its next segment boundary and exits instead of warming the new model.
    std::atomic<int> epoch_{0};
    // One replay at a time; a superseded one moves its thread object to `retired_`, which only
    // the destructor joins — the loader thread that supersedes a replay must never wait behind
    // the turn line the old thread may hold.
    std::mutex worker_m_;
    std::thread worker_;
    std::vector<std::thread> retired_;
};

} // namespace bmoe::server
