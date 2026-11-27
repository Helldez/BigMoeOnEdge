// The writer of one streamed chat turn, which keeps the stream from falling silent and stops the
// turn once its client is gone.
//
// A turn has long stretches with nothing to send: the prefill of a long prompt, and a tool call,
// whose arguments only go out once the turn is parsed (a whole file written by an agent is
// thousands of tokens). How long a stretch lasts is the machine's speed times the model's length,
// so no bound on it holds; a client or proxy that drops a stream silent for a few minutes then
// drops the turn and asks again, and the same prompt brings the same silence. So silence is
// bounded by time, not by tokens: when nothing was written for `interval`, a thread of this
// writer sends the protocol's keep-alive frame. Every write goes through here, under one lock, so
// a keep-alive never lands inside another frame.
//
// A write that fails raises the turn's stop (stop_flag(), EngineHost::chat's `stop`), which the
// engine reads at its cancellation points, inside a prefill too, where no token comes to be
// refused. A write alone finds a client that left late: the first one after it left still goes
// out, and only its reset makes the next fail, which in a silent prefill is up to two intervals
// on. So once per `look` the thread also asks whether the connection reads closed (`closed`:
// the HTTP library's peek, which sees an end of stream or an error), and then sends the keep-alive
// at once. A client that is gone resets it, and the next look's write fails. A client that only
// closed its sending side, its request sent (a half-close), reads at the same end of stream: it
// takes the frames as any keep-alive, and after kProbes of them went through the thread stops
// asking, so its turn runs on. The flag is this turn's alone, read only while this request's own
// turn runs, so a client that leaves never stops another's.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace bmoe::server {

class StreamKeepalive {
public:
    // `write` sends bytes to the client and returns false once it cannot (the client is gone).
    using Write = std::function<bool(const std::string &)>;
    // Whether the connection reads closed: an end of stream or an error, never a block.
    using Closed = std::function<bool()>;

    // Keep-alives sent on a connection that reads closed before the thread stops asking: a client
    // that is gone resets the first, so the second fails.
    static constexpr int kProbes = 2;

    StreamKeepalive(Write write,
                    std::string keepalive,
                    std::chrono::milliseconds interval,
                    Closed closed = nullptr,
                    std::chrono::milliseconds look = std::chrono::seconds{1})
        : write_(std::move(write)), keepalive_(std::move(keepalive)), interval_(interval), closed_(std::move(closed)),
          look_(look), last_(std::chrono::steady_clock::now()), thread_([this] { run(); }) {}

    ~StreamKeepalive() { stop(); }

    StreamKeepalive(const StreamKeepalive &) = delete;
    StreamKeepalive & operator=(const StreamKeepalive &) = delete;

    // Sends `frames` (nothing when empty) and returns whether the client still reads. After a
    // failed write nothing more is sent.
    bool write(const std::string & frames) {
        std::lock_guard<std::mutex> g(m_);
        send(frames);
        return alive();
    }

    bool alive() const { return !gone_.load(std::memory_order_relaxed); }

    // The turn's stop: raised by the first write that fails and never reset. It lives as long as
    // this writer.
    const std::atomic<bool> * stop_flag() const { return &gone_; }

    // Ends the keep-alives; call before the stream is closed. Idempotent.
    void stop() {
        {
            std::lock_guard<std::mutex> g(m_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    void send(const std::string & frames) {
        if (!alive() || frames.empty()) return;
        if (!write_(frames)) gone_.store(true, std::memory_order_relaxed);
        last_ = std::chrono::steady_clock::now();
    }

    void run() {
        std::unique_lock<std::mutex> g(m_);
        auto next_look = last_ + look_;
        while (!stopping_ && alive()) {
            const bool looking = closed_ && probes_ < kProbes;
            // A write since the wait began moves the deadline: the wake re-reads it.
            const auto due = looking ? std::min(last_ + interval_, next_look) : last_ + interval_;
            if (cv_.wait_until(g, due, [this] { return stopping_; })) break;
            const auto now = std::chrono::steady_clock::now();
            if (looking && now >= next_look) {
                next_look = now + look_;
                // dt:srv.stream.closed
                if (closed_()) {
                    ++probes_;
                    send(keepalive_);
                    continue;
                }
            }
            if (now >= last_ + interval_) send(keepalive_);
        }
    }

    Write write_;
    const std::string keepalive_;
    const std::chrono::milliseconds interval_;
    const Closed closed_;
    const std::chrono::milliseconds look_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::atomic<bool> gone_{false};
    int probes_ = 0;
    std::chrono::steady_clock::time_point last_;
    std::thread thread_; // last: started once every other member is set
};

} // namespace bmoe::server
