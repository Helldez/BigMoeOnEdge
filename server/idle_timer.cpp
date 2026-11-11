#include "idle_timer.h"

#include <utility>

namespace bmoe::server {

IdleTimer::IdleTimer(std::function<void()> fire, Clock now, bool threaded)
    : fire_(std::move(fire)), now_(std::move(now)), threaded_(threaded) {}

IdleTimer::~IdleTimer() {
    stop();
}

void IdleTimer::touch(std::chrono::milliseconds delay) {
    std::lock_guard<std::mutex> lk(m_);
    if (stopped_) return;
    pending_ = true;
    due_ = now_() + delay;
    if (threaded_ && !thread_.joinable()) thread_ = std::thread([this] { loop(); });
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

bool IdleTimer::take_due_locked() {
    if (stopped_ || !pending_ || now_() < due_) return false;
    pending_ = false;
    return true;
}

bool IdleTimer::poll() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!take_due_locked()) return false;
    }
    fire_();
    return true;
}

void IdleTimer::loop() {
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        cv_.wait(lk, [this] { return stopped_ || pending_; });
        if (stopped_) return;
        // A touch while waiting moves the due time on: wait again until the latest one.
        const auto due = due_;
        cv_.wait_until(lk, due, [this, due] { return stopped_ || !pending_ || due_ != due; });
        if (!take_due_locked()) continue;
        lk.unlock();
        fire_();
        lk.lock();
    }
}

} // namespace bmoe::server
