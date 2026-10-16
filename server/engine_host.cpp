#include "engine_host.h"

#include "decide_protocol.h"

#include "bmoe/params.h"
#include "bmoe/version.h"

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

// Errors after which the session is still usable (the request was bad, not the context).
bool recoverable(const std::string & error) {
    return error.find("exceeds the session n_ctx") != std::string::npos ||
           error.find("empty prompt") != std::string::npos ||
           error.find("the chat template rejected the conversation") != std::string::npos ||
           error.find("a message list needs chat mode") != std::string::npos;
}

} // namespace

EngineHost::EngineHost(EventBus & bus) : bus_(bus) {}

EngineHost::~EngineHost() {
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
        std::shared_ptr<Session> s = Session::open(session_config_from(effective), error);
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
                              {"streaming", effective.moe.enabled}};
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
    std::shared_ptr<Session> s;
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
    bus_.publish("state", info());
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
        out.error = "busy: too many requests are already waiting for the model";
        return out;
    case TurnQueue::Admit::Abandoned:
        out.status = 499; // never sent: the client that would read it is gone
        out.error = "the client stopped waiting";
        return out;
    }
    struct Leave {
        TurnQueue & q;
        ~Leave() { q.leave(); }
    } leave{turns_};
    std::lock_guard<std::mutex> g(gen_m_);

    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (state_ == State::Ready) s = session_;
    }
    if (!s) {
        out.status = 503;
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
    RunResult r = s->generate(
        gr,
        [&](const TokenMetrics & tm) {
            bus_.publish("token", token_json(tm));
            if (on_token) on_token(tm);
        },
        nullptr);
    generating_ = false;

    if (!r) {
        out.error = r.error;
        if (recoverable(r.error)) {
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
        out.error = "busy: too many requests are already waiting for the model";
        return out;
    case TurnQueue::Admit::Abandoned:
        out.status = 499;
        out.error = "the client stopped waiting";
        return out;
    }
    struct Leave {
        TurnQueue & q;
        ~Leave() { q.leave(); }
    } leave{turns_};
    std::lock_guard<std::mutex> g(gen_m_);

    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (state_ == State::Ready) s = session_;
    }
    if (!s) {
        out.status = 503;
        out.error = "no model is loaded";
        return out;
    }

    generating_ = true;
    publish_state();
    DecideResult r = s->decide(req);
    generating_ = false;

    if (!r.ok && !r.cancelled) {
        out.error = r.error;
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
