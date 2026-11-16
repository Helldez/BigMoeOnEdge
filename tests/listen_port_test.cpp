// Unit tests for the port the server listens on (server/listen_port.h): which ports a fallback tries,
// the probe for a port something already answers on, and the port file a launcher reads.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG, which compiles assert out.

#include "listen_port.h"

#include <cpp-httplib/httplib.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace bmoe::server;

static int failures = 0;

static void check(bool ok, const std::string & what) {
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

static void fallback_tests() {
    const std::set<int> taken = {8765, 8766};
    std::vector<int> asked;
    const int got = take_first_port(8765, kPortFallbackTries, [&](int p) {
        asked.push_back(p);
        return !taken.count(p);
    });
    check(got == 8767 && asked == std::vector<int>{8765, 8766, 8767},
          "fallback: the next free port after the taken, asked in order, none skipped");
    check(take_first_port(8765, kPortFallbackTries, [](int) { return true; }) == 8765,
          "fallback: the preferred port when it is free");
    check(take_first_port(8765, 1, [](int p) { return p != 8765; }) == 0,
          "fallback: off (one try), a taken port is not moved past");
    int tries = 0;
    check(take_first_port(8765, kPortFallbackTries, [&](int) { return ++tries, false; }) == 0 &&
              tries == kPortFallbackTries,
          "fallback: none free in reach, none, after exactly the tries allowed");
    check(take_first_port(kPortMax, kPortFallbackTries, [](int p) { return p != kPortMax; }) == 0,
          "fallback: never past the last port");
    check(take_first_port(0, kPortFallbackTries, [](int) { return true; }) == 0,
          "fallback: port 0 is the system's choice, never searched from");
}

static void probe_host_tests() {
    check(probe_host("127.0.0.1") == "127.0.0.1", "probe host: a specific address is probed as such");
    check(probe_host("0.0.0.0") == "127.0.0.1", "probe host: the IPv4 wildcard, through IPv4 loopback");
    check(probe_host("::") == "::1", "probe host: the IPv6 wildcard, through IPv6 loopback");
}

// The probe binds nothing: a socket it left behind would hold the port it found free.
static void probe_tests() {
    int port = 0;
    {
        httplib::Server free_port;
        port = free_port.bind_to_any_port("127.0.0.1");
        free_port.stop();
    }
    check(port > 0, "probe: a free port to try");
    check(!port_answers("127.0.0.1", port), "probe: nothing answers on a free port");
    check(!port_answers("127.0.0.1", port), "probe: still nothing after a probe, so it kept nothing");
    httplib::Server held;
    held.Get("/", [](const httplib::Request &, httplib::Response & res) { res.set_content("x", "text/plain"); });
    check(held.bind_to_port("127.0.0.1", port), "probe: another server takes the port");
    std::thread serve([&] { held.listen_after_bind(); });
    held.wait_until_ready();
    check(port_answers("127.0.0.1", port), "probe: a port another server answers on is taken");
    held.stop();
    serve.join();
}

static void port_file_tests() {
    check(port_file_text(8765) == "8765\n", "port file: the digits and a newline");
    check(parse_port_file("8765\n") == 8765, "port file: read back");
    check(parse_port_file("8765") == 0, "port file: no newline is a file caught half written");
    check(parse_port_file("") == 0 && parse_port_file("\n") == 0, "port file: empty holds no port");
    check(parse_port_file("0\n") == 0 && parse_port_file("65536\n") == 0 && parse_port_file("999999\n") == 0,
          "port file: out of range holds no port");
    check(parse_port_file("87a5\n") == 0 && parse_port_file("-1\n") == 0 && parse_port_file(" 8765\n") == 0,
          "port file: anything but digits holds no port");
    const fs::path dir = fs::temp_directory_path() / "bmoe_listen_port_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const std::string path = (dir / "server.port").u8string();
    check(read_port_file(path) == 0, "port file: none written, no port");
    std::ofstream(fs::u8path(path), std::ios::binary) << port_file_text(9123);
    check(read_port_file(path) == 9123, "port file: the port written");
    fs::remove_all(dir, ec);
}

int main() {
    fallback_tests();
    probe_host_tests();
    probe_tests();
    port_file_tests();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
