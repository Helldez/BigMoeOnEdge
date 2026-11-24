// A read of a routed expert that fails must end the run, in every read mode. The serial path used to
// return false to a caller that dropped it: the entry was already counted resident, the matmul ran
// on a slice nobody had read, the answer was reported as a success, and the entry was served as a
// hit on later tokens. The real engine runs over the real fixture; only pread can be told to fail.
#include "bmoe/session.h"

#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <string>

namespace {
std::atomic<bool> failing{false};
std::atomic<long> refused{0};
int check(bool ok, const std::string & label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    return ok ? 0 : 1;
}
} // namespace

ssize_t expert_read_test_pread(int fd, void * buf, size_t count, off_t off) {
    if (failing.load()) {
        ++refused;
        errno = EIO;
        return -1;
    }
    return pread(fd, buf, count, off);
}

int main(int argc, char ** argv) {
    if (argc != 2) return 2;
    struct Mode {
        const char * name;
        int io_threads;
        int cache_mb;
        bool overlap;
    };
    int failures = 0;
    // No cache reads every routed expert on every token; a cache of one MiB reads the misses and
    // counts the entry resident before its read, which is the state the dropped result left behind.
    for (const Mode & mode :
         {Mode{"serial, one lane, no cache", 1, 0, false}, Mode{"serial, four lanes, no cache", 4, 0, false},
          Mode{"serial, four lanes, cached", 4, 1, false}, Mode{"overlap, four lanes, cached", 4, 1, true}}) {
        failing = false;
        refused = 0;
        bmoe::SessionConfig cfg;
        cfg.model_path = argv[1];
        cfg.n_threads = 2;
        cfg.n_ctx = 256;
        cfg.n_batch = 128;
        cfg.n_ubatch = 64;
        cfg.device_use = bmoe::DeviceUse::CpuOnly;
        cfg.moe.enabled = true;
        cfg.moe.cache_mb = mode.cache_mb;
        cfg.moe.force_cache = true;
        cfg.moe.io_threads = mode.io_threads;
        cfg.moe.overlap = mode.overlap;
        std::string error;
        auto session = bmoe::Session::open(cfg, error);
        if (!session) {
            std::fprintf(stderr, "open failed (%s): %s\n", mode.name, error.c_str());
            ++failures;
            continue;
        }
        const std::string name = mode.name;
        bmoe::GenerateRequest request;
        request.prompt = "Hello";
        request.n_predict = 8;
        auto before = session->generate(request, {}, nullptr);
        failures += check(before.ok && before.summary.n_generated == request.n_predict, name + ": reads succeed");

        failing = true;
        request.prompt = "The quick brown fox jumps over the lazy dog";
        auto during = session->generate(request, {}, nullptr);
        failures += check(refused.load() > 0, name + ": a read was refused");
        failures += check(!during.ok, name + ": a refused read fails the answer");
        failures += check(during.error.find("expert stream I/O failed") != std::string::npos,
                          name + ": the failure names the expert stream (" + during.error + ")");

        // Sticky, as docs/session.md states: the cache holds entries whose bytes never arrived.
        failing = false;
        auto after = session->generate(request, {}, nullptr);
        failures += check(!after.ok, name + ": the session stays failed once reads work again");
    }
    return failures ? 1 : 0;
}
