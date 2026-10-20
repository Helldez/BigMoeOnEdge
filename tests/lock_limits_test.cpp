// Failed ceiling queries must not disable a bounded attempt to lock already owned pages. Exercise
// the adapter with denied queries on Apple, and the common bound arithmetic on every host.
#include "platform_io.h"

#include <cstdio>

#if defined(__APPLE__)
#include <cerrno>
#include <cstring>
#include <sys/resource.h>
#include <sys/sysctl.h>

namespace {
struct Queries {
    bool active = false;
    bool user_known = false;
    uint64_t user = 0;
    bool global_known = false;
    uint64_t global = 0;
    bool memlock_known = true;
    rlim_t memlock = RLIM_INFINITY;
} queries;

int test_sysctlbyname(const char * name, void * oldp, size_t * oldlenp, void * newp, size_t newlen) {
    if (!queries.active) return ::sysctlbyname(name, oldp, oldlenp, newp, newlen);
    bool known;
    uint64_t value;
    if (std::strcmp(name, "vm.user_wire_limit") == 0) {
        known = queries.user_known;
        value = queries.user;
    } else if (std::strcmp(name, "vm.global_user_wire_limit") == 0) {
        known = queries.global_known;
        value = queries.global;
    } else {
        return ::sysctlbyname(name, oldp, oldlenp, newp, newlen);
    }
    if (!known) {
        errno = EPERM;
        return -1;
    }
    if (!oldp || !oldlenp || *oldlenp < sizeof(value)) {
        errno = ENOMEM;
        return -1;
    }
    std::memcpy(oldp, &value, sizeof(value));
    *oldlenp = sizeof(value);
    return 0;
}

int test_getrlimit(int resource, struct rlimit * limit) {
    if (!queries.active || resource != RLIMIT_MEMLOCK) return ::getrlimit(resource, limit);
    if (!queries.memlock_known) {
        errno = EPERM;
        return -1;
    }
    limit->rlim_cur = queries.memlock;
    limit->rlim_max = queries.memlock;
    return 0;
}
} // namespace

// This executable supplies the platform object itself. The archive's copy is not linked, so the
// query failures reach the production adapter without adding a test seam or mutating host limits.
#define sysctlbyname test_sysctlbyname
#define getrlimit test_getrlimit
#include "../core/src/io/platform_io.cpp"
#undef getrlimit
#undef sysctlbyname
#endif

int main() {
    using namespace bmoe::pio;
    constexpr uint64_t MiB = 1ull << 20;
    int failures = 0;
    const auto check = [&](bool ok, const char * what) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++failures;
    };
    const auto same = [](LockLimit a, uint64_t bytes, bool known) { return a.bytes == bytes && a.known == known; };

    check(same(intersect_lock_limits({}, {lock_unbounded, true}), lock_unbounded, false),
          "a denied ceiling and reported infinity remain unknown, not zero");
    check(same(intersect_lock_limits({}, {8 * MiB, true}), 8 * MiB, false),
          "an independent finite ceiling survives a denied query without becoming a measured grant");
    check(same(intersect_lock_limits({8 * MiB, true}, {4 * MiB, true}), 4 * MiB, true),
          "independent finite ceilings intersect");
    check(same(intersect_lock_limits({}, {0, true}), 0, true) && same(intersect_lock_limits({0, true}, {}), 0, true),
          "an established zero remains authoritative when another query is denied");
    check(same(remaining_lock_limit({8 * MiB, true}, 3 * MiB, true), 5 * MiB, true),
          "reported usage is deducted from a finite global ceiling");
    check(same(remaining_lock_limit({8 * MiB, true}, 9 * MiB, true), 0, true),
          "usage at or above the global ceiling saturates at zero");
    check(same(remaining_lock_limit({8 * MiB, true}, 0, false), 8 * MiB, false),
          "missing global usage preserves the upper bound and its uncertainty");
    check(same(remaining_lock_limit({0, true}, 0, false), 0, true),
          "an established global zero does not depend on the usage query");
    check(same(remaining_lock_limit({lock_unbounded, true}, MiB, true), lock_unbounded, true),
          "reported infinity is not converted into a huge finite grant by subtraction");
    check(same(remaining_lock_limit({lock_unbounded, true}, 0, false), lock_unbounded, true),
          "an unlimited global ceiling does not depend on a usage query");

#if defined(__APPLE__)
    queries.active = true;
    check(same(lockable_limit(), lock_unbounded, false) && same(lock_in_place_limit(), lock_unbounded, false),
          "Apple EPERM wire queries with RLIM_INFINITY report unknown ceilings");
    check(pinned_max_bytes() == 0, "an unreadable per-buffer ceiling does not enable automatic dense pinning");
    queries.memlock = 8 * MiB;
    check(same(lockable_limit(), 8 * MiB, false), "Apple EPERM wire queries preserve a finite RLIMIT_MEMLOCK");
    queries.memlock = 0;
    check(same(lockable_limit(), 0, true), "Apple finite-zero RLIMIT_MEMLOCK forbids attempts despite denied sysctls");
    queries.memlock = RLIM_INFINITY;
    queries.user_known = true;
    queries.user = 0;
    check(same(lockable_limit(), 0, true), "Apple reported per-user zero remains authoritative");
    queries.user = 8 * MiB;
    queries.memlock = 4 * MiB;
    check(same(lockable_limit(), 4 * MiB, false), "Apple independently known per-user and process ceilings intersect");
    queries.user_known = false;
    queries.global_known = true;
    queries.global = 0;
    check(same(lockable_limit(), 0, true), "Apple reported global zero remains authoritative");
    queries.global_known = false;
    queries.memlock_known = false;
    check(same(lockable_limit(), lock_unbounded, false), "Apple failed RLIMIT and sysctl queries remain unknown");
    queries.active = false;
#endif

    return failures ? 1 : 0;
}
