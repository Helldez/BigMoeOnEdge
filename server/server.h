// bmoe-server as a library: the engine, its policy and the HTTP API in one call that runs until
// the server stops. `bmoe-server` the executable is main() over it; an app that may not start a
// process (iOS) calls it on a thread of its own and speaks the same API to loopback.
#pragma once

namespace bmoe::server {

// Parses `argv` like the executable (`--help` lists the options), serves, and returns the exit
// code once the server has stopped.
int run(int argc, char ** argv);

} // namespace bmoe::server
