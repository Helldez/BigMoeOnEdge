// The one thread that runs every computation of the engine a server hosts.
//
// A compute backend may keep a team of worker threads per calling thread: ggml's CPU backend does when
// it is built with OpenMP, as it is on Linux and Windows. bmoe-cli calls the engine from one thread and
// has one team, whose workers spin between ops and start the next one at once. A server called it from
// every thread a request, a load, a plan or a calibration ran on, and each left a team behind: on an
// EPYC with two 24 GB cards, 122 more threads a request. With more workers than cores the runtime stops
// spinning and parks them between ops, and every op then waits for its workers to wake: the same plan of
// GLM-4.7 decoded at 9.2 tok/s in the server and 16.4 in the CLI, with 20 cores busy against 60. So the
// server hands every computation to this thread, as the CLI runs it: one team, whatever the backend,
// the platform or the model.
//
// run() queues `f`, waits for it and returns its result (or rethrows what it threw). Called on this
// thread itself, from inside a job, it runs `f` at once: a job that asks for another never waits on
// itself. Jobs run one at a time, in the order they were queued.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace bmoe::server {

class EngineThread {
public:
    EngineThread() : worker_([this] { loop(); }) {}
    ~EngineThread() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }
    EngineThread(const EngineThread &) = delete;
    EngineThread & operator=(const EngineThread &) = delete;

    template <class F> std::invoke_result_t<F &> run(F && f) {
        using R = std::invoke_result_t<F &>;
        if (std::this_thread::get_id() == worker_.get_id()) return f();
        std::packaged_task<R()> task([&f]() -> R { return f(); });
        std::future<R> done = task.get_future();
        {
            std::lock_guard<std::mutex> lk(m_);
            jobs_.emplace_back([&task] { task(); });
        }
        cv_.notify_one();
        return done.get();
    }

    // Whether the caller is this thread.
    bool here() const { return std::this_thread::get_id() == worker_.get_id(); }

private:
    void loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return stop_ || !jobs_.empty(); });
                if (jobs_.empty()) return; // stopping, nothing left
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
    }

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    bool stop_ = false;
    std::thread worker_; // last: the loop it starts reads the members above
};

} // namespace bmoe::server
