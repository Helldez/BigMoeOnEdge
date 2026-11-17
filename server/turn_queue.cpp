#include "turn_queue.h"

#include <algorithm>

namespace bmoe::server {

void TurnQueue::skip_gone_locked() {
    for (;;) {
        auto it = std::find(gone_.begin(), gone_.end(), serving_);
        if (it == gone_.end()) return;
        gone_.erase(it);
        ++serving_;
    }
}

TurnQueue::Admit TurnQueue::enter(const std::function<bool()> & still_wanted, std::chrono::milliseconds poll) {
    std::unique_lock<std::mutex> lk(m_);
    // Everyone holding a ticket not yet served, less the one running and those who left the line.
    const size_t in_line = (size_t) (next_ticket_ - serving_) - gone_.size();
    if (in_line > max_waiting_) return Admit::Full;
    const uint64_t ticket = next_ticket_++;
    while (serving_ != ticket) {
        cv_.wait_for(lk, poll);
        if (serving_ == ticket) break;
        if (!still_wanted) continue;
        lk.unlock(); // the callback may write to a socket: never under the lock
        const bool wanted = still_wanted();
        lk.lock();
        if (wanted || serving_ == ticket) continue;
        gone_.push_back(ticket);
        return Admit::Abandoned;
    }
    return Admit::Granted;
}

void TurnQueue::leave() {
    {
        std::lock_guard<std::mutex> lk(m_);
        ++serving_;
        skip_gone_locked();
    }
    cv_.notify_all();
}

size_t TurnQueue::waiting() const {
    std::lock_guard<std::mutex> lk(m_);
    const size_t in_line = (size_t) (next_ticket_ - serving_) - gone_.size();
    return in_line > 0 ? in_line - 1 : 0;
}

} // namespace bmoe::server
