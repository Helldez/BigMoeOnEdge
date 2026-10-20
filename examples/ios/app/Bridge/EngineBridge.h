// The one door from the app's Swift into the engine: bmoe-server as a library (server/server.h),
// started on a thread of this process, since an app on iOS may not start a process. Everything
// else the app does goes through the server's HTTP API on loopback, like every other UI.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts the server with these arguments (the executable's own, `--help` lists them) on a thread
// of its own, and returns at once. `exited` is called with run()'s exit code once the server has
// stopped or refused to start, on that thread.
void bmoe_server_start(int argc, const char * const * argv, void (^exited)(int code));

// Asks the server to stop: run() returns and `exited` follows. One still starting stops as soon as
// it listens. False when none is running or starting.
bool bmoe_server_stop(void);

#ifdef __cplusplus
}
#endif
