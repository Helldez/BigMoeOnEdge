// Fan-out of server events to every connected /api/events stream.
//
// Each subscriber owns a bounded queue of ready-to-send SSE frames. Publishing never blocks on a
// slow client: past the backlog bound the oldest frames are dropped, which for a live dashboard is
// the right loss (a stale token row is worth nothing once newer ones exist).
#pragma once

#include "json_util.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bmoe::server {

class EventBus {
public:
    struct Queue {
        std::mutex m;
        std::condition_variable cv;
        std::deque<std::string> frames;
        bool closed = false;
    };

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
    std::mutex m_;
    std::vector<std::shared_ptr<Queue>> subs_;
};

} // namespace bmoe::server
