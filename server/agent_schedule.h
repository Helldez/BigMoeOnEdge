// When a task runs: once, every so many minutes, daily or on chosen weekdays at a time of day, all
// in the machine's local time. next_run() is a pure function of the schedule and two instants, so
// the scheduler's whole calendar is tested without waiting for one (tests/agent_test.cpp).
#pragma once

#include "json_util.h"

#include <ctime>
#include <string>
#include <vector>

namespace bmoe::server::agent {

// The shortest interval a task may repeat at: a model's turn takes tens of seconds, and a task that
// runs faster than it finishes only queues behind itself.
inline constexpr int k_min_interval_minutes = 5;

struct Schedule {
    std::string kind = "once"; // once | interval | daily | weekly
    // once: a local date and time, "YYYY-MM-DDTHH:MM" (what a datetime-local field gives);
    // daily, weekly: a local time of day, "HH:MM"; interval: an optional first run, as for once.
    std::string at;
    int every_minutes = 0;     // interval
    std::vector<int> weekdays; // weekly: 0 = Sunday .. 6 = Saturday, as a JavaScript Date counts them
};

// False with the reason for a schedule that cannot run.
bool parse_schedule(const json & j, Schedule & out, std::string & err);
json schedule_json(const Schedule & s);

// The first time after `after` (Unix seconds) the schedule says to run, or 0 when it never runs
// again. `after` is the last run, or the task's creation before its first: a time that passed while
// the server was not running is still returned, and the task runs once as soon as it is seen.
std::time_t next_run(const Schedule & s, std::time_t after, bool ran_before);

} // namespace bmoe::server::agent
