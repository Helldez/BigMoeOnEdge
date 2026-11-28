// The cancellation policy used by every native Session::open placement, including its fallback.
// No model or backend: a placement is a callable, and cancellation is raised at exact boundaries.
#include "../core/src/engine/opening_stop.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>

using namespace bmoe;

static int failures = 0;
static void check(bool ok, const char * what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

struct Opened {
    explicit Opened(int & count) : alive(count) { ++alive; }
    ~Opened() { --alive; }
    int & alive;
};

int main() {
    std::atomic<bool> stop{false}, unrelated{true};
    SessionConfig outer, fallback;
    outer.stop = &stop;
    fallback.stop = &unrelated; // an explicit fallback may have been constructed by another caller
    std::string error;
    int alive = 0, attempts = 0;
    const auto open = [&](const SessionConfig & cfg) {
        ++attempts;
        check(cfg.stop == &stop && !cfg.stop->load(), "a fallback borrows the outer open's live stop flag");
        return std::make_unique<Opened>(alive);
    };
    {
        auto s = detail::open_attempt(outer, fallback, error, open);
        check(s && alive == 1 && attempts == 1, "an unstopped placement is kept");
    }
    check(alive == 0, "an accepted placement has ordinary ownership");

    stop = true;
    auto s = detail::open_attempt(outer, fallback, error, open);
    check(!s && attempts == 1 && error == "the load was stopped", "a stopped open starts no further placement");

    stop = false;
    error.clear();
    s = detail::open_attempt(outer, fallback, error, [&](const SessionConfig & cfg) {
        ++attempts;
        check(cfg.stop == &stop, "cancellation during a fallback reaches the same operation");
        stop = true;
        return std::make_unique<Opened>(alive); // finishes despite cancellation, as a driver may
    });
    check(!s && alive == 0 && error == "the load was stopped", "a late successful placement is destroyed, not kept");

    stop = false;
    error.clear();
    s = detail::open_attempt(outer, fallback, error, [&](const SessionConfig &) {
        error = "allocation refused";
        return std::unique_ptr<Opened>{};
    });
    check(!s && error == "allocation refused", "an ordinary placement failure keeps its reason");

    outer.stop = nullptr;
    s = detail::open_attempt(outer, fallback, error, [&](const SessionConfig & cfg) {
        check(!cfg.stop, "a fallback never imports an unrelated stop when the caller supplied none");
        return std::make_unique<Opened>(alive);
    });
    check(s && alive == 1, "the optional stop preserves opens with no cancellation flag");
    return failures ? 1 : 0;
}
