#include "cpu_thermal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <set>
#include <utility>

namespace fs = std::filesystem;

namespace bmoe::server {

struct CpuThermal::Zone {
    fs::path temp;
    std::set<double> trips;
    bool seen = false;
    double first = 0.0;
    bool moved = false;
};

namespace {

// Kernel thermal `temp` is conventionally millidegrees, but some vendors report tenths or degrees.
double normalise(double raw) {
    if (raw > 1000.0) return raw / 1000.0;
    if (raw > 200.0) return raw / 10.0;
    return raw;
}

bool read_celsius(const fs::path & p, double & out) {
    std::ifstream f(p);
    std::string text;
    if (!f || !std::getline(f, text)) return false;
    char * end = nullptr;
    const double raw = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) return false;
    out = normalise(raw);
    return out >= 1.0 && out <= 150.0;
}

bool names_cpu(const fs::path & zone) {
    std::ifstream f(zone / "type");
    std::string type;
    if (!f || !std::getline(f, type)) return false;
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return type.find("cpu") != std::string::npos;
}

// The number after "thermal_zone", or -1 for a name without one: those sort last.
long zone_number(const std::string & name) {
    const std::string digits = name.substr(std::string("thermal_zone").size());
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); }))
        return -1;
    return std::strtol(digits.c_str(), nullptr, 10);
}

} // namespace

CpuThermal::CpuThermal(std::string root) : root_(std::move(root)) {}
CpuThermal::~CpuThermal() = default;

std::vector<CpuThermal::Zone> CpuThermal::discover() const {
    std::vector<std::pair<long, fs::path>> dirs;
    std::error_code ec;
    if (root_.empty()) return {};
    for (fs::directory_iterator it(fs::u8path(root_), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().u8string();
        if (name.rfind("thermal_zone", 0) != 0) continue;
        const long n = zone_number(name);
        dirs.emplace_back(n < 0 ? std::numeric_limits<long>::max() : n, it->path());
    }
    std::sort(dirs.begin(), dirs.end());
    std::vector<Zone> zones;
    for (const auto & [n, dir] : dirs) {
        if (!names_cpu(dir)) continue;
        Zone z;
        z.temp = dir / "temp";
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().u8string();
            const std::string suffix = "_temp";
            if (name.rfind("trip_point_", 0) != 0 || name.size() < suffix.size() ||
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
                continue;
            double t = 0.0;
            if (read_celsius(it->path(), t)) z.trips.insert(t);
        }
        zones.push_back(std::move(z));
    }
    return zones;
}

double CpuThermal::sample() {
    std::lock_guard<std::mutex> lk(m_);
    if (!discovered_) {
        zones_ = discover();
        discovered_ = true;
    }
    std::vector<double> all, moving;
    for (Zone & z : zones_) {
        double t = 0.0;
        if (!read_celsius(z.temp, t) || z.trips.count(t)) continue;
        if (!z.seen) {
            z.seen = true;
            z.first = t;
        } else if (t != z.first) {
            z.moved = true;
        }
        all.push_back(t);
        if (z.moved) moving.push_back(t);
    }
    if (all.empty()) return -1.0;
    if (!moving.empty()) return std::accumulate(moving.begin(), moving.end(), 0.0) / (double) moving.size();
    std::sort(all.begin(), all.end());
    const size_t mid = all.size() / 2;
    return all.size() % 2 ? all[mid] : (all[mid - 1] + all[mid]) / 2.0;
}

} // namespace bmoe::server
