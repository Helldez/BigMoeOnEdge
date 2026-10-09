#include "event_bus.h"

#include <algorithm>

namespace bmoe::server {

std::shared_ptr<EventBus::Queue> EventBus::subscribe() {
    auto q = std::make_shared<Queue>();
    std::lock_guard<std::mutex> lk(m_);
    subs_.push_back(q);
    return q;
}

void EventBus::unsubscribe(const std::shared_ptr<Queue> & q) {
    std::lock_guard<std::mutex> lk(m_);
    subs_.erase(std::remove(subs_.begin(), subs_.end(), q), subs_.end());
}

std::string EventBus::frame(const std::string & event, const json & data) {
    return "event: " + event + "\ndata: " + data.dump(-1, ' ', false, json::error_handler_t::replace) + "\n\n";
}

void EventBus::publish(const std::string & event, const json & data) {
    const std::string f = frame(event, data);
    std::vector<std::shared_ptr<Queue>> subs;
    {
        std::lock_guard<std::mutex> lk(m_);
        subs = subs_;
    }
    for (const auto & q : subs) {
        {
            std::lock_guard<std::mutex> lk(q->m);
            if (q->closed) continue;
            if (q->frames.size() >= kMaxBacklog) q->frames.pop_front();
            q->frames.push_back(f);
        }
        q->cv.notify_one();
    }
}

void EventBus::close_all() {
    std::vector<std::shared_ptr<Queue>> subs;
    {
        std::lock_guard<std::mutex> lk(m_);
        subs = subs_;
    }
    for (const auto & q : subs) {
        {
            std::lock_guard<std::mutex> lk(q->m);
            q->closed = true;
        }
        q->cv.notify_all();
    }
}

bool EventBus::next(Queue & q, std::string & out, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(q.m);
    q.cv.wait_for(lk, timeout, [&] { return q.closed || !q.frames.empty(); });
    if (q.closed) return false;
    out.clear();
    if (!q.frames.empty()) {
        out = std::move(q.frames.front());
        q.frames.pop_front();
    }
    return true;
}

} // namespace bmoe::server
