// What the server needs from the operating system. The only file in server/ with platform #ifs.
#pragma once

#include "json_util.h"

#include <atomic>
#include <cstddef>
#include <string>

namespace bmoe::server::platform {

// {ram_total_mib, ram_available_mib, cpu_threads}; a field the platform cannot report is 0.
json host_info();

// Directory holding the running executable, or "" if it cannot be determined.
std::string executable_dir();

// Per-user data directory for settings and downloaded models (created on first use):
// %LOCALAPPDATA%\BigMoeOnEdge, ~/Library/Application Support/BigMoeOnEdge,
// or $XDG_DATA_HOME/bigmoeonedge (~/.local/share/bigmoeonedge).
std::string default_data_dir();

// What this platform's compute backends are told before any of them is loaded, each only where
// the environment has not said otherwise. On Android: the Hexagon NPU backend waits for the DSP by
// polling and offers its host buffer type, which the prefill device moves the KV cache into. Called
// first thing, so a device-probe child inherits it.
void prepare_compute_backends(const std::string & data_dir);

// Open `url` in the default browser. Best effort.
void open_url(const std::string & url);

// True when a file manager started this process in a console of its own (a double click on
// Windows): nobody is there to read a URL, so the server opens the browser itself.
bool started_by_double_click();

// The signed-in user's home folder, or "" when there is none (a phone app's sandbox has no such
// thing a person edits): where the settings files of other programs live (connect.h).
std::string home_dir();

// The operating system, in the words a system prompt tells a model ("macOS", "Linux", "Windows",
// "Android", "iOS"), so it writes commands for the shell it will get.
const char * os_name();

// Whether the agent may start a command here. False on Android and iOS: an app there may not start
// arbitrary programs (iOS forbids it; Android has no shell a person's tools live in), so run_command
// is not offered at all rather than offered and refused.
bool can_run_commands();

// What one command did.
struct CommandResult {
    bool started = false; // false: `error` says why nothing ran
    int exit_code = -1;   // the program's own; -1 when it was killed or did not end on its own
    bool timed_out = false;
    bool stopped = false;   // `stop` was raised while it ran
    bool truncated = false; // the output passed `max_output` and the rest was read and dropped
    std::string output;     // stdout and stderr, interleaved as written, at most `max_output` bytes
    std::string error;
};

// Run `command` through the platform's shell (`/bin/sh -c`, `cmd /c` on Windows) in `cwd`, with
// stdin closed. The command and everything it starts run as one process group (a job on Windows),
// killed whole when `timeout_s` passes or `stop` turns true, so a server stop never leaves a
// command behind. Blocks until it ends.
CommandResult run_command(const std::string & command,
                          const std::string & cwd,
                          int timeout_s,
                          size_t max_output,
                          const std::atomic<bool> & stop);

} // namespace bmoe::server::platform
