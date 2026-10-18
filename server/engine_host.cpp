#include "engine_host.h"

#include "decide_protocol.h"

#include "bmoe/log.h"
#include "bmoe/params.h"
#include "bmoe/version.h"

#include <cstdio>
#include <filesystem>
#include <utility>

namespace bmoe::server {

namespace {

const char * state_name(int s) {
    static const char * names[] = {"empty", "loading", "ready", "error"};
    return names[s];
}

// Chat turns and decisions wait in the same line and hold the same lock: the context is one.
struct Admission {
    int status = 0;
    std::string error;
};

const char * level_name(LogLevel l) {
    return l == LogLevel::Error ? "error" : l == LogLevel::Warn ? "warn" : "info";
}

// A Session, as the host sees it.
class RealSession final : public HostedSession {
public:
    explicit RealSession(std::unique_ptr<Session> s) : s_(std::move(s)) {}
    RunResult generate(const GenerateRequest & req,
                       const std::function<void(const TokenMetrics &)> & on_token) override {
        return s_->generate(req, on_token, nullptr);
    }
    DecideResult decide(const DecideRequest & req) override { return s_->decide(req); }
    void cancel() override { s_->cancel(); }
    const std::string & arch() const override { return s_->arch(); }
    int n_ctx() const override { return s_->n_ctx(); }
    int n_expert_used() const override { return s_->n_expert_used(); }
    ThinkControl think_control() const override { return s_->think_control(); }
    double load_seconds() const override { return s_->load_seconds(); }

private:
    std::unique_ptr<Session> s_;
};

} // namespace

std::unique_ptr<HostedSession> open_real_session(const SessionConfig & cfg, std::string & error) {
    std::unique_ptr<Session> s = Session::open(cfg, error);
    if (!s) return nullptr;
    return std::make_unique<RealSession>(std::move(s));
}

// The bytes the default sink writes, so the server's stderr reads as it always has, and the same
// line on the bus for a UI. Runs under the log port's lock, on whichever engine thread logged.
void EngineHost::log_to_bus(LogLevel level, const char * message, void * self) {
    std::fprintf(stderr, "bmoe: %s\n", message);
    static_cast<EngineHost *>(self)->bus_.publish("log", json{{"level", level_name(level)}, {"message", message}});
}

EngineHost::EngineHost(EventBus & bus, SessionOpener open) : bus_(bus), open_(std::move(open)) {
    set_log_sink(&EngineHost::log_to_bus, this);
}

EngineHost::~EngineHost() {
    set_log_sink(nullptr); // before anything goes: the sink reaches into bus_
    cancel();
    if (loader_.joinable()) loader_.join();
    std::lock_guard<std::mutex> g(gen_m_);
    std::lock_guard<std::mutex> lk(m_);
    session_.reset();
}

void EngineHost::set_host_info(std::function<json()> provider) {
    host_info_ = std::move(provider);
}

void EngineHost::begin_planning() {
    planning_ = true;
    publish_state();
}

void EngineHost::end_planning() {
    planning_ = false;
    publish_state();
}

std::string EngineHost::load(const RunConfig & effective, const RunConfig & requested) {
    const ValidationResult v = validate(effective);
    if (!v) return v.error;
    std::lock_guard<std::mutex> life(life_m_);
    {
        std::lock_guard<std::mutex> lk(m_);
        if (state_ == State::Loading) return "a model is already loading";
        state_ = State::Loading;
        error_.clear();
        model_ = nullptr;
        requested_ = requested;
    }
    if (loader_.joinable()) loader_.join(); // the previous load finished: its state was not Loading
    publish_state();
    cancel();

    loader_ = std::thread([this, effective] {
        // Wait out any generation, then drop the old session BEFORE opening the new one: two large
        // models resident at once is exactly the memory this engine exists to avoid spending.
        std::lock_guard<std::mutex> g(gen_m_);
        {
            std::lock_guard<std::mutex> lk(m_);
            session_.reset();
        }
        std::string error;
        std::shared_ptr<HostedSession> s = open_(session_config_from(effective), error);
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!s) {
                state_ = State::Error;
                error_ = error.empty() ? "the model failed to load" : error;
            } else {
                session_ = s;
                state_ = State::Ready;
                model_ = json{{"path", effective.model_path},
                              {"name", std::filesystem::u8path(effective.model_path).filename().u8string()},
                              {"arch", s->arch()},
                              {"n_ctx", s->n_ctx()},
                              {"n_expert_used", s->n_expert_used()},
                              {"think_ctl", think_control_name(s->think_control())},
                              {"load_s", s->load_seconds()},
                              {"streaming", effective.moe.enabled},
                              // What a CPU figure is read against: the compute threads, and every
                              // thread busy in a decode, the read lanes included when they run
                              // beside compute. A UI reads these instead of the parameters.
                              {"compute_threads", effective.n_threads},
                              {"busy_threads",
                               effective.n_threads +
                                   (effective.moe.enabled && effective.moe.overlap ? effective.moe.io_threads : 0)}};
            }
        }
        publish_state();
    });
    return "";
}

void EngineHost::unload() {
    std::lock_guard<std::mutex> life(life_m_);
    cancel();
    if (loader_.joinable()) loader_.join();
    {
        std::lock_guard<std::mutex> g(gen_m_);
        std::lock_guard<std::mutex> lk(m_);
        session_.reset();
        state_ = State::Empty;
        error_.clear();
        model_ = nullptr;
    }
    publish_state();
}

void EngineHost::cancel() {
    std::shared_ptr<HostedSession> s;
    {
        std::lock_guard<std::mutex> lk(m_);
        s = session_;
    }
    if (s) s->cancel();
}

bool EngineHost::ready() const {
    std::lock_guard<std::mutex> lk(m_);
    return state_ == State::Ready;
}

bool EngineHost::loaded_or_loading() const {
    std::lock_guard<std::mutex> lk(m_);
    return state_ == State::Ready || state_ == State::Loading;
}

std::string EngineHost::model_name() const {
    std::lock_guard<std::mutex> lk(m_);
    return model_.is_object() ? model_.value("name", "") : "";
}

bool EngineHost::reload_required(const RunConfig & current) const {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ != State::Ready) return false;
    for (const ParamDesc & d : params())
        if (d.scope == ParamScope::Session && d.get(requested_) != d.get(current)) return true;
    return false;
}

json EngineHost::info() const {
#if defined(BMOE_HAVE_EXPERT_READY_HOOK)
    const bool overlap = true;
#else
    const bool overlap = false;
#endif
    std::lock_guard<std::mutex> lk(m_);
    return json{{"version", version()},
                {"overlap_available", overlap},
                {"planner_available", planner_available_.load()},
                {"host", host_info_ ? host_info_() : json::object()},
                {"state", planning_.load() && state_ == State::Empty ? "planning" : state_name((int) state_)},
                {"error", error_},
                {"model", model_},
                {"generating", generating_.load()}};
}

void EngineHost::publish_state() const {
    state_cv_.notify_all();
    bus_.publish("state", info());
}

std::unique_lock<std::mutex> EngineHost::take_turn() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(m_);
            state_cv_.wait(lk, [&] { return state_ != State::Loading; });
        }
        std::unique_lock<std::mutex> g(gen_m_);
        std::lock_guard<std::mutex> lk(m_);
        if (state_ != State::Loading) return g;
    }
}

ChatOutcome EngineHost::chat(const ChatRequest & req,
                             const std::function<void(const TokenMetrics &)> & on_token,
                             const std::function<bool()> & still_wanted) {
    ChatOutcome out;
    switch (turns_.enter(still_wanted)) {
    case TurnQueue::Admit::Granted:
        break;
    case TurnQueue::Admit::Full:
        out.status = 429;
        out.code = error_code::busy;
        out.error = "busy: too many requests are already waiting for the model";
        return out;
    case TurnQueue::Admit::Abandoned:
        out.status = 499; // never sent: the client that would read it is gone
        out.code = error_code::client_gone;
        out.error = "the client stopped waiting";
        return out;
    }
    struct Leave {
        TurnQueue & q;
        ~Leave() { q.leave(); }
    } leave{turns_};
    std::unique_lock<std::mutex> g = take_turn();

    std::shared_ptr<HostedSession> s;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (state_ == State::Ready) s = session_;
    }
    if (!s) {
        out.status = 503;
        out.code = error_code::no_model;
        out.error = "no model is loaded";
        return out;
    }

    // The client's conversation is the conversation: the session takes it whole and reuses whatever
    // of it the KV already holds, so there is nothing here to compare, replay or drop.
    GenerateRequest gr;
    gr.messages = req.messages;
    gr.tools = req.tools;
    gr.tool_choice = req.tool_choice;
    gr.clear_kv = false;
    gr.n_predict = req.n_predict;
    gr.think = req.think;
    gr.render_text = true;

    generating_ = true;
    publish_state();
    RunResult r = s->generate(gr, [&](const TokenMetrics & tm) {
        bus_.publish("token", token_json(tm));
        if (on_token) on_token(tm);
    });
    generating_ = false;

    if (!r) {
        out.error = r.error;
        out.code = run_error_name(r.error_kind);
        if (run_error_recoverable(r.error_kind)) {
            out.status = 400;
        } else {
            // A failed decode leaves the context in an unknown state: the session is not reused.
            out.status = 500;
            std::lock_guard<std::mutex> lk(m_);
            session_.reset();
            state_ = State::Error;
            error_ = "the session failed and was closed: " + r.error;
            model_ = nullptr;
        }
        publish_state();
        return out;
    }

    out.n_predict = gr.n_predict;
    out.result = std::move(r);
    bus_.publish("done", summary_json(out.result.summary, out.result.cancelled));
    publish_state();
    return out;
}

DecideOutcome EngineHost::decide(const DecideRequest & req, const std::function<bool()> & still_wanted) {
    DecideOutcome out;
    switch (turns_.enter(still_wanted)) {
    case TurnQueue::Admit::Granted:
        break;
    case TurnQueue::Admit::Full:
        out.status = 429;
        out.code = error_code::busy;
        out.error = "busy: too many requests are already waiting for the model";
        return out;
    case TurnQueue::Admit::Abandoned:
        out.status = 499;
        out.code = error_code::client_gone;
        out.error = "the client stopped waiting";
        return out;
    }
    struct Leave {
        TurnQueue & q;
        ~Leave() { q.leave(); }
    } leave{turns_};
    std::unique_lock<std::mutex> g = take_turn();

    std::shared_ptr<HostedSession> s;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (state_ == State::Ready) s = session_;
    }
    if (!s) {
        out.status = 503;
        out.code = error_code::no_model;
        out.error = "no model is loaded";
        return out;
    }

    generating_ = true;
    publish_state();
    DecideResult r = s->decide(req);
    generating_ = false;

    if (!r.ok && !r.cancelled) {
        out.error = r.error;
        out.code = run_error_name(r.error_kind);
        if (!r.fatal) {
            out.status = 400;
        } else {
            out.status = 500;
            std::lock_guard<std::mutex> lk(m_);
            session_.reset();
            state_ = State::Error;
            error_ = "the session failed and was closed: " + r.error;
            model_ = nullptr;
        }
        publish_state();
        return out;
    }
    out.result = std::move(r);
    bus_.publish("decide", decide_json(out.result));
    publish_state();
    return out;
}

} // namespace bmoe::server
