// Fan-out of server events to every connected /api/events stream.
//
// Each subscriber owns a bounded queue of ready-to-send SSE frames. Publishing never blocks on a
// slow client: past the backlog bound the oldest frames are dropped, which for a live dashboard is
// the right loss (a stale token row is worth nothing once newer ones exist).
//
// Each subscriber is also an HTTP worker held for as long as its stream is open, so how many there
// may be is bounded too: past the bound, subscribe() refuses and the server answers 503 rather than
// let idle streams take every worker and leave the UI's own requests queued behind them.
#pragma once

#include "json_util.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bmoe::server {

// How many event streams may be open at once. Every open UI (a browser tab, a phone app) holds one
// for its lifetime; eight is more tabs than a person watches at once, and small enough that the
// streams, the running chat turn and the turns waiting behind it still leave the server's workers
// room to answer everything else (the pool is sized from this, in http_server.cpp).
inline constexpr size_t k_max_event_streams = 8;

class EventBus {
public:
    explicit EventBus(size_t max_subscribers = k_max_event_streams) : max_subs_(max_subscribers) {}

    struct Queue {
        std::mutex m;
        std::condition_variable cv;
        std::deque<std::string> frames;
        bool closed = false;
    };

    // A new subscriber, or nullptr when `max_subscribers` are subscribed already.
    std::shared_ptr<Queue> subscribe();
    void unsubscribe(const std::shared_ptr<Queue> & q);

    void publish(const std::string & event, const json & data);

    // Wake every subscriber and make next() report closed: the server is stopping.
    void close_all();

    // The next frame for `q`. Returns false once the bus is closed; on timeout returns true with
    // `out` empty, so the caller can send a keep-alive.
    static bool next(Queue & q, std::string & out, std::chrono::milliseconds timeout);

    static std::string frame(const std::string & event, const json & data);

private:
    static constexpr size_t kMaxBacklog = 4096;
    const size_t max_subs_;
    std::mutex m_;
    std::vector<std::shared_ptr<Queue>> subs_;
};

} // namespace bmoe::server
