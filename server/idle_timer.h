// A call made once a quiet period has passed: each touch() starts the wait again, so the call comes
// only after nothing has touched it for the whole period. The server's load-time calibration waits
// for its idle point this way (http_server.cpp, kCalibrationIdleDelay): an answer's end touches it,
// and the next answer, a moment later, touches it again before it fires.
//
// The clock is injected: the thread waits on the real one, and a test drives a timer with no thread
// on a clock of its own, calling poll() at the instants it chooses, so what it checks never depends
// on how fast the machine runs.
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace bmoe::server {

class IdleTimer {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    // `fire` runs on the timer's thread (or in poll()), never twice at once. `threaded` false: no
    // thread is made, and the timer fires only from poll().
    explicit IdleTimer(
        std::function<void()> fire, Clock now = [] { return std::chrono::steady_clock::now(); }, bool threaded = true);
    ~IdleTimer(); // stop()

    // Fire once `delay` has passed on the clock with no other touch.
    void touch(std::chrono::milliseconds delay);
    // Forget a pending fire.
    void cancel();
    // Forget it, wait for a fire in progress to end, and end the thread; touch() is then ignored.
    void stop();
    // Fire now if the quiet period has passed: true when it fired.
    bool poll();

private:
    void loop();
    bool take_due_locked(); // pending and past due: no longer pending

    std::function<void()> fire_;
    Clock now_;
    const bool threaded_;
    std::mutex m_;
    std::condition_variable cv_;
    std::thread thread_;
    bool pending_ = false;
    bool stopped_ = false;
    std::chrono::steady_clock::time_point due_{};
};

} // namespace bmoe::server
