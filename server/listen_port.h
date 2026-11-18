// Which port the server listens on, and how the program that started it learns which.
//
// One policy for every front-end: the server takes the port it is given, or, asked to fall back, the
// first free one from there, and writes the port it took to a file the launcher named. A launcher
// (the desktop app, a phone app, a service supervisor) never guesses a port and never probes one
// itself: a probe followed by a start is a race another program can win, and a launcher that
// assumed the port would speak, key and all, to whatever holds it.
#pragma once

#include <functional>
#include <string>

namespace bmoe::server {

inline constexpr int kPortMax = 65535;

// How many ports from the preferred one a fallback tries, the preferred one included. Enough to step
// past the handful of services that crowd a popular port, few enough that a server never wanders far
// from where a person expects it.
inline constexpr int kPortFallbackTries = 20;

// The ports from `preferred` on, `tries` of them at most and none past kPortMax, handed in order to
// `take`, which holds the port and answers true, or answers false and holds nothing. Returns the port
// taken, 0 when none was. `preferred` is never 0 here: port 0 asks the system for any free port.
int take_first_port(int preferred, int tries, const std::function<bool(int port)> & take);

// The address a probe for `bind_host` connects to: the host itself, or the loopback address of its
// family when it is a wildcard (every address includes loopback).
std::string probe_host(const std::string & bind_host);

// Whether something already listens on `host`:`port`. A port can be bound twice over on some stacks
// (a specific address beside a wildcard one), so a successful bind alone does not prove the port is
// free: whoever answers there would get the connections meant for the server. A connect that is
// refused or times out found nothing.
bool port_answers(const std::string & host, int port);

// The port file: the port as decimal digits and a newline, written once the socket is bound, through
// a temporary file and a rename, so a reader sees either no file or the whole number.
std::string port_file_text(int port);
// The port in a port file's text; 0 when the text is not one (absent, partial, out of range).
int parse_port_file(const std::string & text);
// The port in the file at `path`; 0 when there is no such file or it does not hold one.
int read_port_file(const std::string & path);

} // namespace bmoe::server
