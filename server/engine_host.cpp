#include "engine_host.h"

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

// Errors after which the session is still usable (the request was bad, not the context).
bool recoverable(const std::string & error) {
    return error.find("exceeds the session n_ctx") != std::string::npos ||
           error.find("empty prompt") != std::string::npos;
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

std::string EngineHost::load(const RunConfig & effective, const RunConfig & requested) {
    const ValidationResult v = validate(effective);
    if (!v) return v.error;
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
        history_.clear();
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
    cancel();
    if (loader_.joinable()) loader_.join();
    {
        std::lock_guard<std::mutex> g(gen_m_);
        history_.clear();
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
                {"state", state_name((int) state_)},
                {"error", error_},
                {"model", model_},
                {"generating", generating_.load()}};
}

void EngineHost::publish_state() const {
    bus_.publish("state", info());
}

ChatOutcome EngineHost::chat(const ChatRequest & req, const std::function<void(const TokenMetrics &)> & on_token) {
    ChatOutcome out;
    std::unique_lock<std::mutex> g(gen_m_, std::try_to_lock);
    if (!g) {
        out.status = 409;
        out.error = "busy: a generation is already running";
        return out;
    }
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

    const TurnPlan turn = plan_turn(history_, req.messages);
    if (!turn.ok) {
        out.status = 400;
        out.error = turn.error;
        return out;
    }
    out.history_dropped = turn.history_dropped;

    GenerateRequest gr;
    gr.prompt = turn.prompt;
    gr.clear_kv = turn.clear_kv;
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
        history_.clear();
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

    // What the KV now holds, in the form the client received it: the next request is compared
    // against this, and it will carry the answer back exactly as it was sent.
    history_ = req.messages;
    history_.push_back({"assistant", utf8_as_sent(r.generated_text)});
    out.finish_reason = r.cancelled ? "cancelled" : r.summary.n_generated >= gr.n_predict ? "length" : "stop";
    out.result = std::move(r);
    bus_.publish("done", summary_json(out.result.summary, out.result.cancelled));
    publish_state();
    return out;
}

} // namespace bmoe::server
