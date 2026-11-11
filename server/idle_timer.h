// A call made once a quiet period has passed: each touch() starts the wait again, so the call comes
// only after nothing has touched it for the whole period. The server's load-time calibration waits
// for its idle point this way (http_server.cpp, kCalibrationIdleDelay): an answer's end touches it,
// and the next answer, a moment later, touches it again before it fires.
//
// Pure policy over a thread and a clock: no session, no HTTP, so it is tested on its own.
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace bmoe::server {

class IdleTimer {
public:
    // `fire` runs on the timer's own thread, never twice at once.
    explicit IdleTimer(std::function<void()> fire);
    ~IdleTimer(); // stop()

    // Fire once `delay` has passed with no other touch. The first touch makes the thread.
    void touch(std::chrono::milliseconds delay);
    // Forget a pending fire.
    void cancel();
    // Forget it, wait for a fire in progress to end, and end the thread; touch() is then ignored.
    void stop();

private:
    void loop();

    std::function<void()> fire_;
    std::mutex m_;
    std::condition_variable cv_;
    std::thread thread_;
    bool pending_ = false;
    bool stopped_ = false;
    std::chrono::steady_clock::time_point due_{};
};

} // namespace bmoe::server
