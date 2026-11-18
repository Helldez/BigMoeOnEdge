// What the engine host needs of a loaded model, and how it gets one.
//
// The host's job is the threading around one Session — loads in the background, one turn at a
// time, cancel and unload mid-turn, a failed turn closing the session — and none of that depends on
// what a turn computes. This is the narrowest view of a Session that job uses, so a test can drive
// the host's state machine with a scripted session instead of a model. The real one is a Session,
// opened by open_real_session(); nothing else in the server implements it.
#pragma once

#include "bmoe/session.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bmoe::server {

class HostedSession {
public:
    virtual ~HostedSession() = default;

    // As Session::generate (no metrics sink: the host publishes tokens itself) and Session::decide.
    virtual RunResult generate(const GenerateRequest & req,
                               const std::function<void(const TokenMetrics &)> & on_token) = 0;
    virtual DecideResult decide(const DecideRequest & req) = 0;
    // Thread-safe, as Session::cancel: called while generate() or decide() runs on another thread.
    virtual void cancel() = 0;

    virtual const std::string & arch() const = 0;
    virtual int n_ctx() const = 0;
    virtual int n_expert_used() const = 0;
    virtual ThinkControl think_control() const = 0;
    virtual double load_seconds() const = 0;
    // As Session::vision: whether a request may carry images.
    virtual bool vision() const { return false; }
    // As Session::routing_record: what the model routed so far, for the next plan. Between turns.
    virtual RoutingTrace routing_record() const { return {}; }
    // As Session::measured_knobs: the knob trial's verdicts so far. Between turns.
    virtual MeasuredKnobs measured_knobs() const { return {}; }
    // As Session::tokenize, token_piece, detokenize and render_chat: the model's tokenizer and
    // template, outside a turn and safe beside one. A scripted session has neither.
    virtual std::vector<int32_t> tokenize(const std::string &, bool, bool) const { return {}; }
    virtual std::string token_piece(int32_t) const { return ""; }
    virtual bool detokenize(const std::vector<int32_t> &, std::string &) const { return false; }
    virtual bool render_chat(const GenerateRequest &, std::string &, std::string & error) const {
        error = "this session has no chat template";
        return false;
    }
};

// Opens a session for `cfg`, or returns null and says why in `error`. Called on the loader thread.
using SessionOpener = std::function<std::unique_ptr<HostedSession>(const SessionConfig & cfg, std::string & error)>;

// The opener every server runs with: a bmoe::Session.
std::unique_ptr<HostedSession> open_real_session(const SessionConfig & cfg, std::string & error);

} // namespace bmoe::server
