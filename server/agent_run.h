// The state of one message's run, shared by agent.cpp (the built-in agent's rounds) and
// agent_external.cpp (a conversation whose agent is another program). Internal to the agent.
#pragma once

#include "agent.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace bmoe::server::agent {

// One message's run: its stop and the approvals it waits for. Shared by the run's thread and the
// HTTP workers that approve or stop it.
struct Agent::Run {
    std::atomic<bool> stop{false};
    std::mutex m;
    std::condition_variable cv;
    std::map<std::string, int> decisions;          // item -> -1 waiting, 0 denied, 1 approved; under m
    std::map<std::string, std::string> waiting_in; // item -> the conversation it waits in; under m
    bool done = false;                             // under m
    bool remote = false;
    bool auto_deny = false;
    bool think = false;
    bool failed = false; // run thread only
    int denied = 0;      // run thread only
    std::string root;
    std::string task;
    std::thread thread;
};

} // namespace bmoe::server::agent
