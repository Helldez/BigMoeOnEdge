// The one loaded model, and everything that touches it.
//
// A Session is not thread-safe and serves one generation at a time, so this class owns the rules
// around it: loads happen on a background thread (a large model takes tens of seconds and the UI
// must stay live), a load waits for any running generation to finish, chat turns wait their place
// in a line and run one at a time, and every state change is published on the event bus. It keeps
// no conversation: a client sends its own on every request and the session reuses what the KV holds.
#pragma once

#include "chat_turn.h"
#include "hosted_session.h"
#include "turn_queue.h"
#include "event_bus.h"
#include "json_util.h"

#include "bmoe/config.h"
#include "bmoe/log.h"
#include "bmoe/session.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bmoe::server {

// The stable codes an error carries beside its message, for a client to act on (docs/server-api.md).
// The engine's own failures use run_error_name(); these are the host's refusals before the engine.
namespace error_code {
inline constexpr const char * busy = "busy";               // the line of waiting requests is full
inline constexpr const char * client_gone = "client_gone"; // never sent: the client left the line
inline constexpr const char * no_model = "no_model";       // nothing is loaded to answer
} // namespace error_code

struct ChatOutcome {
    int status = 200;  // HTTP status; not 200 means `error` says why and `result` is empty
    std::string error; // for a person
    std::string code;  // for a client: run_error_name() of the engine's kind, or an error_code
    RunResult result;
    int n_predict = 0; // the budget the turn ran with, for the protocol's stop-or-length answer
};

struct DecideOutcome {
    int status = 200; // as ChatOutcome
    std::string error;
    std::string code;
    DecideResult result;
};

// How many chat turns may wait behind the running one before the next is turned away. A local
// server has a handful of clients; a line longer than this is a client retrying, not a workload.
inline constexpr size_t k_max_waiting_turns = 16;

class EngineHost {
public:
    // `open` makes the session a load installs; a test passes a scripted one (hosted_session.h).
    // The host also routes the engine's diagnostics (bmoe/log.h) to the bus as `log` events, still
    // writing them to stderr as before, for as long as it lives.
    explicit EngineHost(EventBus & bus, SessionOpener open = open_real_session);
    ~EngineHost();

    // What /api/info adds about the machine; called on every state publish.
    void set_host_info(std::function<json()> provider);
    void set_planner_available(bool available) { planner_available_ = available; }

    // A load in auto mode measures the machine before it loads, for tens of seconds on a phone,
    // with nothing loaded meanwhile: `state` reads "planning" between these two calls, so a UI can
    // say what is happening the moment a person asks for a load.
    void begin_planning();
    void end_planning();

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

    // Run one chat turn on the calling thread, after any turn that asked first (turn_queue.h).
    // `on_token` (nullable) sees every token. `still_wanted` (nullable) is asked while the turn
    // waits its place and gives it up by returning false.
    ChatOutcome chat(const ChatRequest & req,
                     const std::function<void(const TokenMetrics &)> & on_token,
                     const std::function<bool()> & still_wanted = nullptr);

    // Read one decision (bmoe/decide.h) on the calling thread, in the same line as the chat turns:
    // one prefill, no decode. A refused request (no choices, colliding choices, a prompt past
    // n_ctx) is a 400 and leaves the session usable; a failed prefill closes it, as a failed turn does.
    DecideOutcome decide(const DecideRequest & req, const std::function<bool()> & still_wanted = nullptr);

private:
    enum class State { Empty, Loading, Ready, Error };

    void publish_state() const;

    // Take gen_m_ for a turn, after any load that has been announced. load() reads Loading before
    // its thread takes the lock, and a turn arriving in that gap would take it first, find no
    // session and answer 503 for a model that is seconds from ready: so a turn that holds the lock
    // while the state still reads Loading has overtaken the loader, and gives the lock back until
    // the load has ended one way or the other.
    std::unique_lock<std::mutex> take_turn();

    static void log_to_bus(LogLevel level, const char * message, void * self);

    EventBus & bus_;
    const SessionOpener open_;
    std::function<json()> host_info_;
    std::atomic<bool> planner_available_{false};
    std::atomic<bool> planning_{false};

    // Held across load() and unload(), so the loader thread is joined and replaced by one caller
    // at a time. Without it two unloads join the same thread, and a load racing an unload assigns
    // onto a thread that is still joinable, which ends the process. The loader itself never takes
    // it, so waiting for a load under it cannot deadlock.
    std::mutex life_m_;

    mutable std::mutex m_;                     // guards everything below except loader_ (see life_m_)
    mutable std::condition_variable state_cv_; // signalled by publish_state(), after any change
    State state_ = State::Empty;
    std::string error_;
    std::shared_ptr<HostedSession> session_;
    RunConfig requested_;
    json model_ = nullptr;
    std::thread loader_;

    std::mutex gen_m_; // held for a whole generation, and by a load while it swaps the session
    std::atomic<bool> generating_{false};
    // Chat turns line up here before they take gen_m_, so they run in the order they arrived; a
    // load takes gen_m_ alone and goes ahead of the line as soon as the running turn ends.
    TurnQueue turns_{k_max_waiting_turns};
};

} // namespace bmoe::server
