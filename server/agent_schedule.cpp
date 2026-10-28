#include "agent_schedule.h"

#include "platform.h"

#include <cstdio>
#include <mutex>

namespace bmoe::server::agent {

namespace {

std::tm local(std::time_t t) {
    return platform::local_time(t);
}

// mktime reads the time zone's process-wide state; the scheduler and a request can both ask.
std::time_t make_time(std::tm & tm) {
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    return std::mktime(&tm);
}

bool parse_hhmm(const std::string & s, int & h, int & m) {
    char extra = 0;
    return std::sscanf(s.c_str(), "%2d:%2d%c", &h, &m, &extra) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60;
}

// "YYYY-MM-DDTHH:MM" as local time, Unix seconds; 0 when it does not parse.
std::time_t parse_local_datetime(const std::string & s) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    char extra = 0;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d%c", &y, &mo, &d, &h, &mi, &extra) != 5) return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59) return 0;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_isdst = -1; // the zone's rule for that date, not today's
    const std::time_t t = make_time(tm);
    return t < 0 ? 0 : t;
}

} // namespace

bool parse_schedule(const json & j, Schedule & out, std::string & err) {
    if (!j.is_object()) {
        err = "schedule must be an object";
        return false;
    }
    Schedule s;
    s.kind = j.value("kind", std::string("once"));
    if (j.contains("at") && j["at"].is_string()) s.at = j["at"].get<std::string>();
    if (j.contains("every_minutes") && j["every_minutes"].is_number_integer())
        s.every_minutes = j["every_minutes"].get<int>();
    if (j.contains("weekdays") && j["weekdays"].is_array())
        for (const json & d : j["weekdays"])
            if (d.is_number_integer() && d.get<int>() >= 0 && d.get<int>() <= 6) s.weekdays.push_back(d.get<int>());
    int h = 0, m = 0;
    if (s.kind == "once") {
        if (!parse_local_datetime(s.at)) {
            err = "a one-time task needs `at` as a local date and time, YYYY-MM-DDTHH:MM";
            return false;
        }
    } else if (s.kind == "interval") {
        if (s.every_minutes < k_min_interval_minutes) {
            err = "every_minutes must be at least " + std::to_string(k_min_interval_minutes);
            return false;
        }
        if (!s.at.empty() && !parse_local_datetime(s.at)) {
            err = "`at` (the first run) must be YYYY-MM-DDTHH:MM";
            return false;
        }
    } else if (s.kind == "daily" || s.kind == "weekly") {
        if (!parse_hhmm(s.at, h, m)) {
            err = "a " + s.kind + " task needs `at` as a local time of day, HH:MM";
            return false;
        }
        if (s.kind == "weekly" && s.weekdays.empty()) {
            err = "a weekly task needs at least one weekday (0 = Sunday .. 6 = Saturday)";
            return false;
        }
    } else {
        err = "schedule kind must be once, interval, daily or weekly";
        return false;
    }
    out = std::move(s);
    return true;
}

json schedule_json(const Schedule & s) {
    return json{{"kind", s.kind}, {"at", s.at}, {"every_minutes", s.every_minutes}, {"weekdays", s.weekdays}};
}

std::time_t next_run(const Schedule & s, std::time_t after, bool ran_before) {
    if (s.kind == "once") return ran_before ? 0 : parse_local_datetime(s.at);
    if (s.kind == "interval") {
        if (!ran_before && !s.at.empty()) return parse_local_datetime(s.at);
        return s.every_minutes > 0 ? after + (std::time_t) s.every_minutes * 60 : 0;
    }
    int h = 0, m = 0;
    if (!parse_hhmm(s.at, h, m)) return 0;
    // Day by day from the day of `after`: the first one with the time of day past `after` (and, for
    // weekly, on a chosen weekday). mktime normalises day 32 into the next month and applies each
    // day's own daylight-saving rule.
    const std::tm base = local(after);
    for (int day = 0; day <= 8; ++day) {
        std::tm tm{};
        tm.tm_year = base.tm_year;
        tm.tm_mon = base.tm_mon;
        tm.tm_mday = base.tm_mday + day;
        tm.tm_hour = h;
        tm.tm_min = m;
        tm.tm_isdst = -1;
        const std::time_t t = make_time(tm);
        if (t <= after) continue;
        if (s.kind == "weekly") {
            bool chosen = false;
            for (int d : s.weekdays)
                chosen = chosen || d == tm.tm_wday;
            if (!chosen) continue;
        }
        return t;
    }
    return 0;
}

} // namespace bmoe::server::agent
