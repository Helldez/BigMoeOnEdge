// The one loaded model, and everything that touches it.
//
// A Session is not thread-safe and serves one generation at a time, so this class owns the rules
// around it: loads happen on a background thread (a large model takes tens of seconds and the UI
// must stay live), a load waits for any running generation to finish, a second generation while
// one runs is refused rather than queued, and every state change is published on the event bus.
// Which turn a request becomes is conversation.h's rule; this class only keeps the history it
// compares against.
#pragma once

#include "conversation.h"
#include "event_bus.h"
#include "json_util.h"

#include "bmoe/config.h"
#include "bmoe/session.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bmoe::server {

struct ChatRequest {
    std::vector<ChatMessage> messages;
    int n_predict = 0;
    bool think = true;
};

struct ChatOutcome {
    int status = 200; // HTTP status; not 200 means `error` says why and `result` is empty
    std::string error;
    RunResult result;
    bool history_dropped = false; // earlier turns could not be replayed into a fresh KV
    std::string finish_reason;    // stop | length | cancelled
};

class EngineHost {
public:
    explicit EngineHost(EventBus & bus);
    ~EngineHost();

    // What /api/info adds about the machine; called on every state publish.
    void set_host_info(std::function<json()> provider);
    void set_planner_available(bool available) { planner_available_ = available; }

    // Open a session with `effective` in the background. `requested` is the config the user asked
    // for, which can differ (a dense model loads with streaming off); it is what reload_required()
    // compares against. Returns an error for a config that cannot start, "" once the load is under
    // way.
    std::string load(const RunConfig & effective, const RunConfig & requested);
    void unload();
    void cancel();

    json info() const;
    bool ready() const;
    bool loaded_or_loading() const;
    bool generating() const { return generating_; }
    std::string model_name() const;

    // True when a session is loaded and `current` differs from it in a session-scoped parameter.
    bool reload_required(const RunConfig & current) const;

    // Run one chat turn on the calling thread. `on_token` (nullable) sees every token.
    ChatOutcome chat(const ChatRequest & req, const std::function<void(const TokenMetrics &)> & on_token);

private:
    enum class State { Empty, Loading, Ready, Error };

    void publish_state() const;

    EventBus & bus_;
    std::function<json()> host_info_;
    std::atomic<bool> planner_available_{false};

    mutable std::mutex m_; // guards everything below except history_
    State state_ = State::Empty;
    std::string error_;
    std::shared_ptr<Session> session_;
    RunConfig requested_;
    json model_ = nullptr;
    std::thread loader_;

    std::mutex gen_m_; // held for a whole generation, and by a load while it swaps the session
    std::atomic<bool> generating_{false};
    std::vector<ChatMessage> history_; // the conversation the KV holds; guarded by gen_m_
};

} // namespace bmoe::server
