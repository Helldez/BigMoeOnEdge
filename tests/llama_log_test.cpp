// The llama.cpp log bridge (core/src/engine/llama_log.h), driven through the callback it installs:
// DEBUG dropped unless asked for, a CONT line following the fate of the message it continues, an
// ERROR demoted only while a cancel the engine requested is in flight, and everything that passes
// forwarded unchanged to the callback that was there before. No model.
#include "llama_log.h"

#include "llama.h"

#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static std::string g_seen;

static void capture(ggml_log_level, const char * text, void *) {
    g_seen += text;
}

int main() {
    using namespace bmoe::detail;
    // What was installed first is what the bridge forwards to: here, a capture.
    llama_log_set(&capture, nullptr);
    install_llama_log();
    install_llama_log(); // idempotent: a second call must not chain the bridge to itself
    ggml_log_callback cb = nullptr;
    void * ud = nullptr;
    llama_log_get(&cb, &ud);
    CHECK(cb != nullptr && cb != &capture);
    const auto say = [&](ggml_log_level l, const char * t) {
        g_seen.clear();
        cb(l, t, ud);
        return g_seen;
    };

    CHECK(say(GGML_LOG_LEVEL_INFO, "info\n") == "info\n");
    CHECK(say(GGML_LOG_LEVEL_WARN, "warn\n") == "warn\n");
    CHECK(say(GGML_LOG_LEVEL_ERROR, "error\n") == "error\n");
    CHECK(say(GGML_LOG_LEVEL_DEBUG, "debug").empty());
    CHECK(say(GGML_LOG_LEVEL_CONT, " more\n").empty()); // continues a dropped line: dropped
    CHECK(say(GGML_LOG_LEVEL_INFO, "info") == "info");
    CHECK(say(GGML_LOG_LEVEL_CONT, " more\n") == " more\n"); // continues a shown line: shown

    set_llama_log_debug(true);
    CHECK(say(GGML_LOG_LEVEL_DEBUG, "debug") == "debug");
    CHECK(say(GGML_LOG_LEVEL_CONT, ".\n") == ".\n");
    set_llama_log_debug(false);

    // A cancel in flight: its decode's ERROR lines are not errors, and nothing else changes.
    llama_log_cancel_begin();
    CHECK(say(GGML_LOG_LEVEL_ERROR, "llama_decode: failed to decode, ret = 2\n").empty());
    CHECK(say(GGML_LOG_LEVEL_CONT, "tail\n").empty());
    CHECK(say(GGML_LOG_LEVEL_WARN, "warn\n") == "warn\n");
    set_llama_log_debug(true); // asked for, they are still there, as debug
    CHECK(!say(GGML_LOG_LEVEL_ERROR, "failed\n").empty());
    set_llama_log_debug(false);
    llama_log_cancel_end();
    CHECK(say(GGML_LOG_LEVEL_ERROR, "error\n") == "error\n"); // window closed: errors are errors again

    // Two windows open, one closed: still cancelling.
    llama_log_cancel_begin();
    llama_log_cancel_begin();
    llama_log_cancel_end();
    CHECK(say(GGML_LOG_LEVEL_ERROR, "e\n").empty());
    llama_log_cancel_end();
    CHECK(say(GGML_LOG_LEVEL_ERROR, "e\n") == "e\n");

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("llama_log: ok\n");
    return 0;
}
