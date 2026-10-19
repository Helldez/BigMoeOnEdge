// The control-group remainder (pio::cgroup_memory_left) over fake hierarchies: v2 and v1 layouts,
// "max", an ancestor tighter than the group, reclaimable file pages, a group over its cap.

#include "platform_io.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

int failures = 0;
std::string tmp;

void check(bool ok, const char * what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

void put(const std::string & path, const std::string & text) {
    for (size_t i = 1; i < path.size(); ++i)
        if (path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0755);
    FILE * f = std::fopen(path.c_str(), "w");
    std::fputs(text.c_str(), f);
    std::fclose(f);
}

std::string fresh(const char * name) {
    const std::string d = tmp + "/" + name;
    ::mkdir(d.c_str(), 0755);
    return d;
}

constexpr unsigned long long GiB = 1ull << 30, MiB = 1ull << 20;

} // namespace

int main() {
    char templ[] = "/tmp/bmoe-cgroup-XXXXXX";
    tmp = ::mkdtemp(templ);
    using bmoe::pio::cgroup_memory_left;

    {
        // A container: the namespace shows its own group as the root.
        const std::string d = fresh("v2-root");
        put(d + "/self", "0::/\n");
        put(d + "/fs/memory.max", std::to_string(85 * GiB) + "\n");
        put(d + "/fs/memory.current", std::to_string(28 * GiB) + "\n");
        put(d + "/fs/memory.stat", "anon 1\ninactive_file " + std::to_string(12 * GiB) + "\nactive_file 5\n");
        const auto left = cgroup_memory_left((d + "/fs").c_str(), (d + "/self").c_str());
        check(left == (85 - 28 + 12) * GiB, "v2: the limit less what is held, crediting inactive file pages");
    }
    {
        const std::string d = fresh("v2-max");
        put(d + "/self", "0::/user.slice/app\n");
        put(d + "/fs/user.slice/app/memory.max", "max\n");
        put(d + "/fs/user.slice/app/memory.current", "123\n");
        check(cgroup_memory_left((d + "/fs").c_str(), (d + "/self").c_str()) == 0,
              "v2: \"max\" everywhere is no limit");
    }
    {
        // The group is unlimited, its parent is not: the parent's cap applies.
        const std::string d = fresh("v2-ancestor");
        put(d + "/self", "0::/box/job\n");
        put(d + "/fs/box/job/memory.max", "max\n");
        put(d + "/fs/box/job/memory.current", std::to_string(1 * GiB) + "\n");
        put(d + "/fs/box/memory.max", std::to_string(8 * GiB) + "\n");
        put(d + "/fs/box/memory.current", std::to_string(6 * GiB) + "\n");
        check(cgroup_memory_left((d + "/fs").c_str(), (d + "/self").c_str()) == 2 * GiB,
              "v2: an ancestor's cap applies");
    }
    {
        const std::string d = fresh("v2-over");
        put(d + "/self", "0::/\n");
        put(d + "/fs/memory.max", std::to_string(512 * MiB) + "\n");
        put(d + "/fs/memory.current", std::to_string(600 * MiB) + "\n");
        check(cgroup_memory_left((d + "/fs").c_str(), (d + "/self").c_str()) == 1,
              "v2: a group at or over its cap has nothing left, and says so rather than \"no limit\"");
    }
    {
        const std::string d = fresh("v1");
        put(d + "/self", "12:cpu,cpuacct:/x\n4:memory:/docker/abc\n");
        put(d + "/fs/memory/docker/abc/memory.limit_in_bytes", std::to_string(16 * GiB) + "\n");
        put(d + "/fs/memory/docker/abc/memory.usage_in_bytes", std::to_string(10 * GiB) + "\n");
        put(d + "/fs/memory/docker/abc/memory.stat", "total_inactive_file " + std::to_string(1 * GiB) + "\n");
        put(d + "/fs/memory/docker/memory.limit_in_bytes", "9223372036854771712\n");
        put(d + "/fs/memory/docker/memory.usage_in_bytes", "1\n");
        check(cgroup_memory_left((d + "/fs").c_str(), (d + "/self").c_str()) == 7 * GiB,
              "v1: the memory controller's limit, its unlimited sentinel ignored");
    }
    {
        const std::string d = fresh("none");
        check(cgroup_memory_left((d + "/fs").c_str(), (d + "/missing").c_str()) == 0,
              "no cgroup file: no limit, not an error");
    }
    std::string rm = "rm -rf " + tmp;
    (void) std::system(rm.c_str());
    return failures ? 1 : 0;
}
