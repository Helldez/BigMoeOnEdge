#include "idle_timer.h"

#include <utility>

namespace bmoe::server {

IdleTimer::IdleTimer(std::function<void()> fire) : fire_(std::move(fire)) {}

IdleTimer::~IdleTimer() {
    stop();
}

void IdleTimer::touch(std::chrono::milliseconds delay) {
    std::lock_guard<std::mutex> lk(m_);
    if (stopped_) return;
    pending_ = true;
    due_ = std::chrono::steady_clock::now() + delay;
    if (!thread_.joinable()) thread_ = std::thread([this] { loop(); });
    cv_.notify_all();
}

void IdleTimer::cancel() {
    std::lock_guard<std::mutex> lk(m_);
    pending_ = false;
    cv_.notify_all();
}

void IdleTimer::stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stopped_ = true;
        pending_ = false;
        cv_.notify_all();
    }
    if (thread_.joinable()) thread_.join();
}

void IdleTimer::loop() {
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        cv_.wait(lk, [this] { return stopped_ || pending_; });
        if (stopped_) return;
        // A touch while waiting moves the due time on: wait again until the latest one.
        const auto due = due_;
        if (cv_.wait_until(lk, due, [this, due] { return stopped_ || !pending_ || due_ != due; })) continue;
        pending_ = false;
        lk.unlock();
        fire_();
        lk.lock();
    }
}

} // namespace bmoe::server
