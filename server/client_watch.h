// The watch over one chat turn answered whole (no stream), which stops the turn once its client is
// gone.
//
// Such a turn writes nothing before its answer, so no failed write shows that its client left, as
// one does on a stream (stream_keepalive.h), and the thread serving the request cannot look: it
// waits in the turn line, then sits inside the engine through the prefill and the generation. So
// once per `look` a thread of the watch asks whether the connection reads closed (`closed`: the
// HTTP library's peek, which sees an end of stream or an error), and then raises the turn's stop
// (stop_flag(), EngineHost::chat's `stop`), which the engine reads at its cancellation points,
// inside a prefill too. A client that only closed its sending side, its request sent (a
// half-close), reads at the same end of stream and counts as gone: before the answer nothing is
// written that could tell the two apart, and the turn line takes a waiting one as gone the same
// way. The flag is this turn's alone, read only while this request's own turn runs, so a client
// that leaves never stops another's.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace bmoe::server {

class ClientWatch {
public:
    // Whether the connection reads closed: an end of stream or an error, never a block.
    using Closed = std::function<bool()>;

    ClientWatch(Closed closed, std::chrono::milliseconds look)
        : closed_(std::move(closed)), look_(look), thread_([this] { run(); }) {}

    // Ends the watch; the turn's stop keeps whatever it was.
    ~ClientWatch() {
        {
            std::lock_guard<std::mutex> g(m_);
            ending_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    ClientWatch(const ClientWatch &) = delete;
    ClientWatch & operator=(const ClientWatch &) = delete;

    // Whether the watch saw the client leave.
    bool gone() const { return gone_.load(std::memory_order_relaxed); }

    // The turn's stop: raised once the connection reads closed and never reset. It lives as long as
    // this watch.
    const std::atomic<bool> * stop_flag() const { return &gone_; }

private:
    void run() {
        std::unique_lock<std::mutex> g(m_);
        while (!cv_.wait_for(g, look_, [this] { return ending_; })) {
            // dt:srv.watch.closed
            if (closed_()) {
                gone_.store(true, std::memory_order_relaxed);
                return;
            }
        }
    }

    const Closed closed_;
    const std::chrono::milliseconds look_;
    std::mutex m_;
    std::condition_variable cv_;
    bool ending_ = false;
    std::atomic<bool> gone_{false};
    std::thread thread_; // last: started once every other member is set
};

} // namespace bmoe::server
