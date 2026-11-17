// Admission to the one generation the engine runs at a time.
//
// A chat client rarely sends one request at a time: an agent asks for a session title while its
// first real turn is already on the way, a UI asks for a summary beside the answer. The engine has
// one context, so those turns run one after another - but a second request is a turn to wait for,
// not an error to return, and a client that gets "busy" retries until it gives up. This is the
// line they wait in: first come, first served, bounded so a stuck client cannot pile work up
// without limit, and a caller that stopped waiting gives its place back.
//
// Pure policy (no session, no HTTP), so it is tested on its own.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace bmoe::server {

class TurnQueue {
public:
    enum class Admit {
        Granted,   // it is this caller's turn; leave() must follow
        Full,      // too many are waiting already; nothing is held
        Abandoned, // `still_wanted` said no while waiting; nothing is held
    };

    // `max_waiting` callers may wait behind the one running; the next is turned away.
    explicit TurnQueue(size_t max_waiting) : max_waiting_(max_waiting) {}

    // Block until every earlier caller has left. While waiting, `still_wanted` (nullable) is asked
    // once per `poll` and ends the wait by returning false: it is how a request whose client went
    // away stops holding a place, and where a streaming endpoint keeps its connection alive.
    Admit enter(const std::function<bool()> & still_wanted = nullptr,
                std::chrono::milliseconds poll = std::chrono::milliseconds(1000));
    void leave();

    size_t waiting() const; // callers in line, not counting the one whose turn it is

private:
    const size_t max_waiting_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    uint64_t next_ticket_ = 0; // the ticket the next caller takes
    uint64_t serving_ = 0;     // the ticket whose turn it is
    // Tickets given up while waiting, so the line moves past them when their turn comes.
    std::vector<uint64_t> gone_;
    void skip_gone_locked();
};

} // namespace bmoe::server
