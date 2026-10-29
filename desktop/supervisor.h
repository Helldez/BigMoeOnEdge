// bmoe-server as the app's child process: started, watched, restarted and stopped. A separate
// process is the crash boundary: a driver that ends the engine ends the server, and the window
// stays to say so and start it again.
#pragma once

#include "app_core.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace bmoe::desktop {

// How long a server is given to answer /api/info after it starts. It probes nothing and loads no
// model at start, so seconds are the norm; the margin is for a first start, when the system
// checks a freshly installed binary and a GPU backend compiles its kernels.
inline constexpr std::chrono::seconds kServerStartTimeout{90};

class ServerSupervisor {
public:
    // `on_exit` is told the exit status when the server ends without being asked to, from a thread
    // of the supervisor's.
    ServerSupervisor(std::string log_path, std::function<void(int status)> on_exit);
    ~ServerSupervisor();

    // Starts the server and waits until it answers on `port`. False, with the reason (the end of
    // its log when it exited), when it did not.
    bool start(const ServerLaunch & launch, int port, std::string & err);
    // Stops the server, if one runs, and waits for it to be gone.
    void stop();
    bool running() const { return running_; }

private:
    struct Child;
    std::string log_path_;
    std::function<void(int)> on_exit_;
    std::mutex m_;
    std::unique_ptr<Child> child_;
    std::thread reader_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> exited_{false};
    std::atomic<int> status_{0};
};

} // namespace bmoe::desktop
