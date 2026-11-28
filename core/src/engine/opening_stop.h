#pragma once

// The stop state of one open, shared by all its placement attempts. Pure policy: the caller
// supplies the placement, so flag inheritance and a late successful open can be tested without
// loading a model or asking a device to fail.
#include "bmoe/session.h"

#include <string>

namespace bmoe::detail {

inline bool opening_stopped(const SessionConfig & cfg, std::string & error) {
    // dt:open.stopped
    if (!cfg.stop || !cfg.stop->load(std::memory_order_relaxed)) return false;
    error = "the load was stopped";
    return true;
}

template <class Open>
auto open_attempt(const SessionConfig & outer, SessionConfig attempt, std::string & error, const Open & open)
    -> decltype(open(attempt)) {
    attempt.stop = outer.stop;
    // dt:open.attempt.before
    if (opening_stopped(attempt, error)) return {};
    auto result = open(attempt);
    // dt:open.attempt.after
    if (opening_stopped(attempt, error)) return {};
    return result;
}

} // namespace bmoe::detail
