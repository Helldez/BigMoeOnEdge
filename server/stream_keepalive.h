// The writer of one streamed chat turn, which keeps the stream from falling silent.
//
// A turn has long stretches with nothing to send: the prefill of a long prompt, and a tool call,
// whose arguments only go out once the turn is parsed (a whole file written by an agent is
// thousands of tokens). How long a stretch lasts is the machine's speed times the model's length,
// so no bound on it holds; a client or proxy that drops a stream silent for a few minutes then
// drops the turn and asks again, and the same prompt brings the same silence. So silence is
// bounded by time, not by tokens: when nothing was written for `interval`, a thread of this
// writer sends the protocol's keep-alive frame. Every write goes through here, under one lock, so
// a keep-alive never lands inside another frame.
#pragma once

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

    StreamKeepalive(Write write, std::string keepalive, std::chrono::milliseconds interval)
        : write_(std::move(write)), keepalive_(std::move(keepalive)), interval_(interval),
          last_(std::chrono::steady_clock::now()), thread_([this] { run(); }) {}

    ~StreamKeepalive() { stop(); }

    StreamKeepalive(const StreamKeepalive &) = delete;
    StreamKeepalive & operator=(const StreamKeepalive &) = delete;

    // Sends `frames` (nothing when empty) and returns whether the client still reads. After a
    // failed write nothing more is sent.
    bool write(const std::string & frames) {
        std::lock_guard<std::mutex> g(m_);
        send(frames);
        return alive_;
    }

    bool alive() const {
        std::lock_guard<std::mutex> g(m_);
        return alive_;
    }

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
        if (!alive_ || frames.empty()) return;
        if (!write_(frames)) alive_ = false;
        last_ = std::chrono::steady_clock::now();
    }

    void run() {
        std::unique_lock<std::mutex> g(m_);
        while (!stopping_ && alive_) {
            // A write since the wait began moves the deadline: the wake re-reads it.
            const auto due = last_ + interval_;
            if (cv_.wait_until(g, due, [this] { return stopping_; })) break;
            if (std::chrono::steady_clock::now() >= last_ + interval_) send(keepalive_);
        }
    }

    Write write_;
    const std::string keepalive_;
    const std::chrono::milliseconds interval_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool stopping_ = false;
    bool alive_ = true;
    std::chrono::steady_clock::time_point last_;
    std::thread thread_; // last: started once every other member is set
};

} // namespace bmoe::server
