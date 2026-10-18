// bmoe-server as a library: the engine, its policy and the HTTP API in one call that runs until
// the server stops. `bmoe-server` the executable is main() over it; an app that may not start a
// process (iOS) calls it on a thread of its own and speaks the same API to loopback.
#pragma once

namespace bmoe::server {

// Where the server listens unless told otherwise: this machine only, on one fixed port, so a client
// that starts the server (an app, a browser bookmark) knows where to find it.
inline constexpr const char * kDefaultHost = "127.0.0.1";
inline constexpr int kDefaultPort = 8765;

// What a chat answers with when nobody has said otherwise. The server's defaults, not the engine's
// (RunConfig keeps the library's own, which the CLI and the gates run at): a conversation needs
// more context and a longer answer than a one-shot prompt.
inline constexpr int kChatContextTokens = 4096;
inline constexpr int kChatAnswerTokens = 1024;

// Parses `argv` like the executable (`--help` lists the options), serves, and returns the exit
// code once the server has stopped.
int run(int argc, char ** argv);

} // namespace bmoe::server
