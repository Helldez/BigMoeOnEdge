#include "listen_port.h"

#include <cpp-httplib/httplib.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace bmoe::server {

int take_first_port(int preferred, int tries, const std::function<bool(int port)> & take) {
    if (preferred <= 0) return 0;
    for (int i = 0; i < tries; ++i) {
        const int port = preferred + i;
        if (port > kPortMax) break;
        if (take(port)) return port;
    }
    return 0;
}

std::string probe_host(const std::string & bind_host) {
    if (bind_host.empty() || bind_host == "0.0.0.0") return "127.0.0.1";
    if (bind_host == "::" || bind_host == "[::]") return "::1";
    return bind_host;
}

bool port_answers(const std::string & host, int port) {
    constexpr time_t kProbeTimeoutS = 1;
    httplib::Client c(host, port);
    c.set_connection_timeout(kProbeTimeoutS, 0);
    c.set_read_timeout(kProbeTimeoutS, 0);
    const auto res = c.Get("/");
    if (res) return true;
    // A connect that times out found nothing answering, as a refused one did: Windows retries a
    // refused loopback connect for about two seconds before it says so, past this timeout. Any other
    // failure (a connection taken and then closed, a reply that is not HTTP) means something holds it.
    return res.error() != httplib::Error::Connection && res.error() != httplib::Error::ConnectionTimeout;
}

std::string port_file_text(int port) {
    return std::to_string(port) + "\n";
}

int parse_port_file(const std::string & text) {
    // Digits and one trailing newline, nothing else: a file caught half written has no newline yet.
    if (text.size() < 2 || text.back() != '\n') return 0;
    const std::string digits = text.substr(0, text.size() - 1);
    if (digits.size() > 5) return 0;
    long port = 0;
    for (const char ch : digits) {
        if (!std::isdigit((unsigned char) ch)) return 0;
        port = port * 10 + (ch - '0');
    }
    return port >= 1 && port <= kPortMax ? (int) port : 0;
}

int read_port_file(const std::string & path) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return 0;
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_port_file(ss.str());
}

} // namespace bmoe::server
