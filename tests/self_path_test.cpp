// pio::self_path (core/src/io/platform_io.h): the file this process was started from, with every
// link resolved. Its folder is where a release's backend modules are looked for, and install.sh puts
// a link to each tool on PATH while the files stay in a folder of their own: a path that named the
// link sent the search to the wrong folder, and a release CLI found no backend at all (#605). So the
// test starts itself again through a link in another folder and asks the child where it is.
// No model, no backend: the property is the path, whatever the search then finds.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.
#include "platform_io.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;

static void check(bool ok, const std::string & what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

static const char * const kPrint = "--print-self-path";

// The first line a command prints, or "" when it could not be run.
static std::string first_line(const std::string & command) {
#if defined(_WIN32)
    // cmd.exe strips the outer pair of quotes from a line that starts with one.
    FILE * p = _popen(("\"" + command + "\"").c_str(), "r");
#else
    FILE * p = popen(command.c_str(), "r");
#endif
    if (!p) return "";
    char buf[4096] = {};
    std::string line = std::fgets(buf, sizeof(buf), p) ? buf : "";
#if defined(_WIN32)
    _pclose(p);
#else
    pclose(p);
#endif
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    return line;
}

int main(int argc, char ** argv) {
    if (argc > 1 && std::string(argv[1]) == kPrint) {
        std::printf("%s\n", bmoe::pio::self_path().c_str());
        return 0;
    }

    const std::string self = bmoe::pio::self_path();
    check(!self.empty(), "the platform names this executable");
    if (self.empty()) return 1;
    std::error_code ec;
    const fs::path real = fs::canonical(fs::path(self), ec);
    check(!ec && real == fs::path(self), "started directly, the path names the file itself");

    // A link in a folder of its own, as an installer's bin folder holds one.
    const fs::path dir =
        fs::temp_directory_path(ec) /
        ("bmoe_self_path_" + std::to_string((long long) std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path link = dir / fs::path(self).filename();
    fs::create_symlink(fs::path(self), link, ec);
    if (ec) {
        // Windows without the privilege to make one: the property cannot be set up here, not failed.
        std::printf("[SKIP] no symbolic link could be made here (%s)\n", ec.message().c_str());
    } else {
        const std::string seen = first_line("\"" + link.string() + "\" " + kPrint);
        check(!seen.empty(), "the link starts the executable");
        check(fs::path(seen) == fs::path(self),
              "started through a link, the path names the file, not the link (" + seen + ")");
    }
    fs::remove_all(dir, ec);

    if (failures) std::printf("%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
