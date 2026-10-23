// Session::open runs the full prefill-device confirmation before a metrics sink exists. A lock
// refusal there must use the model's token-cycle floor, not silently turn the cache into ordinary
// memory. The real adapter and engine run over a loopback RPC device; only mlock's allowance is fake.
#include "bmoe/log.h"
#include "bmoe/probe.h"
#include "bmoe/session.h"
#include "ggml-backend.h"

#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
struct LockedPages {
    std::mutex mutex;
    std::set<uintptr_t> pages;
    size_t limit = std::numeric_limits<size_t>::max();
    size_t refusals = 0;
    size_t first_refusal_bytes = 0;
    size_t successes_after_refusal = 0;
    size_t releases = 0;
};
LockedPages & locked() {
    static LockedPages value;
    return value;
}
size_t page_size() {
    static const size_t value = (size_t) sysconf(_SC_PAGESIZE);
    return value;
}
void forget_locked(const void * p, size_t size) {
    auto & state = locked();
    std::lock_guard<std::mutex> guard(state.mutex);
    const size_t page = page_size();
    const uintptr_t first = (uintptr_t) p / page * page;
    const uintptr_t end = ((uintptr_t) p + size + page - 1) / page * page;
    auto begin = state.pages.lower_bound(first);
    auto finish = state.pages.lower_bound(end);
    state.releases += (size_t) std::distance(begin, finish);
    state.pages.erase(begin, finish);
}
void reset_lock_limit(size_t bytes) {
    auto & state = locked();
    std::lock_guard<std::mutex> guard(state.mutex);
    state.pages.clear();
    state.limit = bytes;
    state.refusals = state.first_refusal_bytes = state.successes_after_refusal = state.releases = 0;
}

int free_port() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    int port = 0;
    if (bind(fd, (sockaddr *) &addr, len) == 0 && getsockname(fd, (sockaddr *) &addr, &len) == 0)
        port = ntohs(addr.sin_port);
    close(fd);
    return port;
}
bool listening(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t) port);
    const bool ok = connect(fd, (sockaddr *) &addr, sizeof(addr)) == 0;
    close(fd);
    return ok;
}
std::string rpc_device() {
    auto reg = ggml_backend_reg_by_name("RPC");
    auto cpu = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
    if (!reg || !cpu) return {};
    using Start = void (*)(const char *, const char *, size_t, size_t, ggml_backend_dev_t *);
    using Add = ggml_backend_reg_t (*)(const char *);
    auto start = (Start) ggml_backend_reg_get_proc_address(reg, "ggml_backend_rpc_start_server");
    auto add = (Add) ggml_backend_reg_get_proc_address(reg, "ggml_backend_rpc_add_server");
    const int port = free_port();
    if (!start || !add || !port) return {};
    const std::string endpoint = "127.0.0.1:" + std::to_string(port);
    std::thread([=] {
        ggml_backend_dev_t devices[] = {cpu};
        start(endpoint.c_str(), nullptr, 2, 1, devices);
    }).detach();
    for (int i = 0; i < 200 && !listening(port); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    if (!listening(port)) return {};
    auto server = add(endpoint.c_str());
    if (!server || !ggml_backend_reg_dev_count(server)) return {};
    ggml_backend_register(server);
    return ggml_backend_dev_name(ggml_backend_reg_dev_get(server, 0));
}

struct Logs {
    std::vector<std::string> lines;
    static void sink(bmoe::LogLevel, const char * message, void * user) {
        ((Logs *) user)->lines.emplace_back(message);
        std::fprintf(stderr, "bmoe: %s\n", message);
    }
    size_t find(const char * part) const {
        for (size_t i = 0; i < lines.size(); ++i)
            if (lines[i].find(part) != std::string::npos) return i;
        return lines.size();
    }
};
struct Metrics : bmoe::IMetricsSink {
    bmoe::RunInfo info;
    void on_run_info(const bmoe::RunInfo & value) override { info = value; }
    void on_token(const bmoe::TokenMetrics &) override {}
    void on_summary(const bmoe::RunSummary &) override {}
};
int check(bool ok, const char * label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    return ok ? 0 : 1;
}
} // namespace

int startup_test_mlock(const void * p, size_t size) {
    auto & state = locked();
    std::lock_guard<std::mutex> guard(state.mutex);
    const size_t page = page_size();
    const uintptr_t first = (uintptr_t) p / page * page;
    const uintptr_t end = ((uintptr_t) p + size + page - 1) / page * page;
    size_t additional = 0;
    for (uintptr_t at = first; at < end; at += page)
        additional += state.pages.count(at) == 0;
    if ((state.pages.size() + additional) * page > state.limit) {
        if (!state.refusals) state.first_refusal_bytes = state.pages.size() * page;
        ++state.refusals;
        errno = ENOMEM;
        return -1;
    }
    if (state.refusals && additional) ++state.successes_after_refusal;
    for (uintptr_t at = first; at < end; at += page)
        state.pages.insert(at);
    return 0;
}
int startup_test_munlock(const void * p, size_t size) {
    forget_locked(p, size);
    return 0;
}
void * startup_test_mmap(void * p, size_t size, int prot, int flags, int fd, off_t off) {
    void * result = mmap(p, size, prot, flags, fd, off);
    if (result != MAP_FAILED && (flags & MAP_FIXED)) forget_locked(result, size);
    return result;
}
int startup_test_munmap(void * p, size_t size) {
    const int rc = munmap(p, size);
    if (!rc) forget_locked(p, size);
    return rc;
}

int main(int argc, char ** argv) {
    if (argc != 2) return 2;
    bmoe::register_backends();
    const std::string model = argv[1];
    const bmoe::ModelProfile profile = bmoe::probe_model(model.c_str());
    if (!profile.ok || !profile.token_cycle_bytes || !profile.n_expert_used) return 2;
    const std::string device = rpc_device();
    if (device.empty()) {
        std::printf("[SKIP] startup pin fit requires the loopback RPC backend\n");
        return 77;
    }

    int failures = 0;
    for (const auto & spec : {std::pair<int, bool>{0, true}, {3, true}, {0, false}}) {
        const int top_k = spec.first ? spec.first : (int) profile.n_expert_used;
        const size_t cycle = profile.token_cycle_bytes / profile.n_expert_used * (size_t) top_k;
        const size_t limit = spec.second ? cycle * 2 : cycle / 2;
        reset_lock_limit(limit);
        bmoe::SessionConfig cfg;
        cfg.model_path = model;
        cfg.n_threads = 2;
        cfg.n_ctx = 256;
        cfg.n_batch = 128;
        cfg.n_ubatch = 64;
        cfg.n_expert_used = spec.first;
        cfg.device_use = bmoe::DeviceUse::CpuOnly;
        cfg.moe.enabled = true;
        cfg.moe.cache_mb = (int) ((profile.expert_bytes + (1u << 20) - 1) >> 20);
        cfg.moe.force_cache = true;
        cfg.moe.cache_pin = true;
        cfg.moe.cache_pin_fit = true;
        cfg.prefill.device = device;
        cfg.prefill.verify = true;
        cfg.prefill.best_effort = false;
        cfg.prefill.slots_on_demand = true;

        Logs logs;
        bmoe::set_log_sink(Logs::sink, &logs);
        std::string error;
        auto session = bmoe::Session::open(cfg, error);
        failures += check(session != nullptr, "real Session::open with confirmation succeeds");
        if (!session) {
            std::fprintf(stderr, "open failed: %s\n", error.c_str());
            bmoe::set_log_sink(nullptr);
            continue;
        }
        const size_t confirmation = logs.find("confirmation: 64 tokens, 31 compared, top token agrees 100%");
        failures += check(confirmation < logs.lines.size(), "the 64-token confirmation ran on the device");
        failures += check(locked().refusals > 0, "the startup confirmation actually reached the fake lock limit");
        if (spec.second) {
            failures +=
                check(locked().first_refusal_bytes >= cycle, "first refusal follows at least one model token cycle");
            failures += check(logs.find("cache budget shrinks") < confirmation,
                              "startup pin-fit shrinks before confirmation and before any metrics sink");
            failures += check(logs.find("less than one token cycle") == logs.lines.size(),
                              "startup keeps the granted cache pinned above the model floor");
            failures += check(locked().successes_after_refusal > 0 && locked().releases > 0,
                              "eviction releases locked pages and later expert commits can pin again");
        } else {
            failures += check(locked().first_refusal_bytes < cycle, "negative control refuses below one token cycle");
            failures += check(logs.find("less than one token cycle") < confirmation &&
                                  logs.find("cache budget shrinks") == logs.lines.size(),
                              "below-cycle refusal keeps the ordinary-memory fallback");
        }
        failures +=
            check(session->n_expert_used() == top_k, "effective routing width preserves model default or override");
        bmoe::GenerateRequest request;
        request.prompt = "Hello";
        request.n_predict = 8;
        Metrics metrics;
        auto result = session->generate(request, {}, &metrics);
        failures += check(result.ok && result.summary.n_generated == request.n_predict,
                          "generation survives the startup refusal");
        failures += check(metrics.info.n_expert_used == top_k &&
                              metrics.info.cache_cycle_mb == (int) ((cycle + (1u << 20) - 1) >> 20),
                          "run metadata reports the same effective width and rounded-up model cycle");
        session.reset();
        failures += check(locked().pages.empty(), "session teardown releases every simulated locked page");
        bmoe::set_log_sink(nullptr);
    }
    return failures ? 1 : 0;
}
