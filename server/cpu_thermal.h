#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bmoe::server {

// The CPU's temperature from the kernel's thermal zones (`<root>/thermal_zone*`, whose `type`
// names the CPU): the mean of its sensors, which reads like other monitors' CPU figure, where the
// hottest one alone reads a few busy cores (#465). Some kernels also publish a threshold as a zone
// named for the CPU: its `temp` is a constant that passes any sanity range. Two facts tell it from
// a sensor without naming any zone: it reads one of its own `trip_point_*_temp` values, and it
// never moves. Until a zone has moved the median stands in, so one constant cannot become the
// figure. Zones are found once, in their numeric order, so the result does not depend on the order
// a directory lists them in (#311).
class CpuThermal {
public:
    // `root` is the platform's thermal-zone directory (platform::thermal_zone_root()); "" where
    // the platform has none, and then every sample is unknown.
    explicit CpuThermal(std::string root);
    ~CpuThermal();

    // Degrees Celsius, or a negative value when no CPU sensor is readable. Safe from any thread.
    double sample();

private:
    struct Zone;
    std::vector<Zone> discover() const;

    std::string root_;
    std::mutex m_;
    bool discovered_ = false;
    std::vector<Zone> zones_;
};

} // namespace bmoe::server
