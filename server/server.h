// bmoe-server as a library: the engine, its policy and the HTTP API in one call that runs until
// the server stops. `bmoe-server` the executable is main() over it; an app that may not start a
// process (iOS) calls it on a thread of its own and speaks the same API to loopback.
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace bmoe::server {

class HttpFetch;

// Where the server listens unless told otherwise: this machine only, on one fixed port, so a client
// that starts the server (an app, a browser bookmark) knows where to find it.
inline constexpr const char * kDefaultHost = "127.0.0.1";
inline constexpr int kDefaultPort = 8765;

// What a chat answers with when nobody has said otherwise. The server's defaults, not the engine's
// (RunConfig keeps the library's own, which the CLI and the gates run at): a conversation needs
// more context and a longer answer than a one-shot prompt.
inline constexpr int kChatContextTokens = 4096;
inline constexpr int kChatAnswerTokens = 1024;

// What an app that hosts the server in its own process supplies beyond the command line. Every
// field is optional: left empty, the server does what the executable does.
struct Embedding {
    // The network a repository is read through (http_fetch.h), given the folder for bytes in
    // flight. Empty: the system's curl, which an app on iOS may not start; the app hands its own
    // platform's HTTP stack instead, with the operating system's certificates either way.
    std::function<std::unique_ptr<HttpFetch>(const std::string & scratch_dir)> fetch;
    // Called once the server listens, with the way to stop it: run() then returns. A process of its
    // own is stopped by ending it; one inside an app is stopped by this, to listen again elsewhere.
    // Called again with an empty function once it no longer listens, before what the stop reaches
    // is gone: from then on the stop it was given must not be called.
    std::function<void(std::function<void()> stop)> listening;
};

// Parses `argv` like the executable (`--help` lists the options), serves, and returns the exit
// code once the server has stopped.
int run(int argc, char ** argv);
int run(int argc, char ** argv, const Embedding & embedding);

} // namespace bmoe::server
