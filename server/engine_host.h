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
#include <chrono>
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
inline constexpr const char * busy = "busy";                 // the line of waiting requests is full
inline constexpr const char * client_gone = "client_gone";   // never sent: the client left the line
inline constexpr const char * no_model = "no_model";         // nothing is loaded to answer
inline constexpr const char * streams_full = "streams_full"; // every event stream place is taken
} // namespace error_code

// An engine message (a config validate() refuses, a model that will not load, a failed turn) as a
// client of this server shows it: the settings it names, by the labels the settings form shows.
// Every engine message that leaves for a client goes through here once; the engine names no flag.
std::string engine_message(const std::string & message);

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

// What /api/info says about a session's context against what a client of the server sends
// (RunConfig::ctx_for_clients): the recommendation, whether `n_ctx` is short of it, the parameter
// that raises it, so a front-end names neither the number nor the key, whether a request was refused
// for the context since the load, and whether a UI should warn: when the context is short and either
// other devices reach the server (its clients are there) or a client has already been refused.
json context_advice(int n_ctx, bool serves_network, bool refused = false);

class EngineHost {
public:
    // `open` makes the session a load installs; a test passes a scripted one (hosted_session.h).
    // The host also routes the engine's diagnostics (bmoe/log.h) to the bus as `log` events, still
    // writing them to stderr as before, for as long as it lives.
    explicit EngineHost(EventBus & bus, SessionOpener open = open_real_session);
    ~EngineHost();

    // What /api/info adds about the machine; called on every state publish.
    void set_host_info(std::function<json()> provider);
    // The CPU's temperature in Celsius (negative: unknown), sampled while a turn generates and
    // sent on its token events as `cpu_temp_c`. Unset or unknown, the field is left out.
    void set_cpu_temperature(std::function<double()> provider);
    void set_planner_available(bool available) { planner_available_ = available; }
    // Bound beyond loopback: the clients a short context refuses are there to be warned about.
    void set_serves_network(bool on) { serves_network_ = on; }
    // Where the loaded model's recorded routing goes after a turn that added to it (the model's file
    // and the record), for the next plan of that file to price its cache on. Set before any load.
    void set_routing_sink(std::function<void(const std::string & model_path, const RoutingTrace & rec)> sink) {
        routing_sink_ = std::move(sink);
    }
    // Where the knob trial's verdicts go when one comes in (the model's file and what the session
    // measured so far, not yet stamped with a machine). Set before any load.
    void set_knobs_sink(std::function<void(const std::string & model_path, const MeasuredKnobs & rec)> sink) {
        knobs_sink_ = std::move(sink);
    }

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

    // A step a load runs on its own thread between closing the old session and opening the new one,
    // with the memory the old one held given back: where a server's load-time calibration opens its
    // candidates (docs/measured-planning.md). It may change the configs the load then opens (the
    // decided plan's). Two signals, kept apart because they end differently: `stop` is raised when a
    // turn or a decision arrives (or cancel() is called), and the load then opens what the step
    // settled, for the turn to run on; `cancelled` is raised by unload() and the host's end, and then
    // nothing is opened and the step must leave every setting as it found it (stop is raised too).
    struct BeforeOpenSignals {
        const std::atomic<bool> & stop;
        const std::atomic<bool> & cancelled;
    };
    using BeforeOpen = std::function<void(RunConfig & effective, RunConfig & requested, const BeforeOpenSignals &)>;
    // load() with `before_open` run first, started only while the host is idle (idle()): ready, no
    // turn running, none waiting. Nothing in flight is cancelled. False when it did not start. Until
    // the new session is open, info() reads "ready" with `calibrating` set and the model that was
    // loaded: the server is busy and still takes requests, each waiting for the load as for any and
    // stopping the step first.
    bool load_when_idle(const RunConfig & effective, const RunConfig & requested, BeforeOpen before_open);
    // Ready, with no turn running or waiting for the model.
    bool idle() const;
    // Called on the thread of a turn or a decision once it has left the line, whatever its outcome:
    // where a front-end looks for the idle point after an answer. Set before any turn.
    void set_turn_end_hook(std::function<void()> hook) { turn_end_ = std::move(hook); }
    void unload();
    void cancel();

    json info() const;
    bool ready() const;
    bool loaded_or_loading() const;
    bool generating() const { return generating_; }
    std::string model_name() const;
    // Whether the loaded model reads images (a projector was paired or named at load).
    bool vision() const;
    // The file of the model loaded or loading, "" when none.
    std::string model_path() const;

    // What the last completed turn of this session measured for decode: s_per_token, cache_hit_pct
    // (-1 with no cache) and tokens; null before the first turn and after a load. The figures a plan
    // predicted are checked against these.
    json last_turn() const;

    // The session-scoped parameters in which `current` differs from the loaded session, in table
    // order; empty when no session is loaded. reload_required() is whether there is any.
    std::vector<std::string> reload_keys(const RunConfig & current) const;
    bool reload_required(const RunConfig & current) const { return !reload_keys(current).empty(); }

    // Run one chat turn on the calling thread, after any turn that asked first (turn_queue.h).
    // `on_token` (nullable) sees every token and returns false once the client that asked for this
    // turn is gone: the host then cancels this turn, and only this one. A front-end never cancels on
    // a failed write itself, since it cannot tell whose turn is running when the write fails (a
    // keep-alive while queued, the last frames after the turn ended). `still_wanted` (nullable) is
    // asked while the turn waits its place and gives it up by returning false. It may write a
    // keepalive, so it is never called concurrently with on_token. `stop` (nullable) is the caller's
    // atomic flag, borrowed until return and forwarded to the engine's cancellation points,
    // including encoding and prefill before any token exists. It never targets another turn.
    ChatOutcome chat(const ChatRequest & req,
                     const std::function<bool(const TokenMetrics &)> & on_token,
                     const std::function<bool()> & still_wanted = nullptr,
                     const std::atomic<bool> * stop = nullptr);

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
    // A request refused with context_full: logged, and marked in the advice so a UI warns even
    // after the warning was dismissed (the client itself may show the refusal as an unknown error).
    void note_context_full(int n_ctx);
    // Hand the session's routing record to the sink when it grew. Called inside a turn (gen_m_).
    void keep_routing(const HostedSession & s);
    // Likewise the knob trial's verdicts, when a new one came in.
    void keep_knobs(const HostedSession & s);

    // Start the loader thread for `effective`, after any before-open step. The caller holds life_m_
    // and has set the state to Loading.
    void start_loader(const RunConfig & effective, BeforeOpen before_open);
    // A turn or a decision arrived: a step before an open stops for it (BeforeOpen).
    void note_request();

    std::function<void()> turn_end_;
    // Raised to stop a before-open step, and to skip the open after it; `calibrating_` while one may
    // run (load_when_idle).
    std::atomic<bool> before_open_stop_{false};
    std::atomic<bool> before_open_cancel_{false};
    std::atomic<bool> calibrating_{false};

    std::function<void(const std::string &, const MeasuredKnobs &)> knobs_sink_;
    int knobs_kept_ = 0; // verdicts in the record last handed on; under gen_m_

    std::function<void(const std::string &, const RoutingTrace &)> routing_sink_;
    uint32_t routing_kept_ = 0; // decoded tokens of the record last handed on; under gen_m_

    EventBus & bus_;
    const SessionOpener open_;
    std::function<json()> host_info_;
    std::function<double()> cpu_temp_;
    std::atomic<bool> planner_available_{false};
    std::atomic<bool> serves_network_{false};
    std::atomic<bool> planning_{false};

    // Held across load() and unload(), so the loader thread is joined and replaced by one caller
    // at a time. Without it two unloads join the same thread, and a load racing an unload assigns
    // onto a thread that is still joinable, which ends the process. The loader itself never takes
    // it, so waiting for a load under it cannot deadlock.
    std::mutex life_m_;

    mutable std::mutex m_;                     // guards everything below except loader_ (see life_m_)
    mutable std::condition_variable state_cv_; // signalled by publish_state(), after any change
    State state_ = State::Empty;
    // When the load under way began (its planning, where it plans first): what `load_elapsed_s`
    // in info() counts from, so every UI shows the same seconds and none keeps a clock of its own.
    std::chrono::steady_clock::time_point load_started_{};
    std::string error_;
    std::shared_ptr<HostedSession> session_;
    RunConfig requested_;
    json model_ = nullptr;
    json last_turn_ = nullptr;
    std::thread loader_;

    std::mutex gen_m_; // held for a whole generation, and by a load while it swaps the session
    std::atomic<bool> generating_{false};
    // Chat turns line up here before they take gen_m_, so they run in the order they arrived; a
    // load takes gen_m_ alone and goes ahead of the line as soon as the running turn ends.
    TurnQueue turns_{k_max_waiting_turns};
};

} // namespace bmoe::server
