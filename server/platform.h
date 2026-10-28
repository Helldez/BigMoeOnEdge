// What the server needs from the operating system. The only file in server/ with platform #ifs.
// Built as a library of its own (bmoe_server_platform) with nothing of the engine in it, so the
// desktop app (desktop/) asks the same functions where the data folder is and how to open a URL.
#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bmoe::server {
using json = nlohmann::ordered_json;
} // namespace bmoe::server

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

// The build's target as the release feed names it ("macos-arm64", "linux-x86_64",
// "windows-arm64", ...), or "" for a platform the releases do not ship (a phone).
const char * build_target();

// Whether the shared library named (a bare file name: "libcuda.so.1", "nvcuda.dll") loads here,
// found where the system's loader looks. Loaded and released at once; nothing of it is called.
bool library_loads(const std::string & name);

// Readies the backend modules about to be loaded from folders other than the executable's (the
// packs, docs/backends.md), so the libraries each carries beside itself are found. On Windows a
// module loaded by its path looks for its own dependencies in the program's folder and the
// system's, not its own: each is loaded first with its folder searched. Elsewhere a module finds
// them through its run path, and this does nothing.
void prepare_backend_modules(const std::vector<std::string> & module_paths);

// The calendar time `t` reads in this machine's time zone, and in UTC. Safe from any thread: the C
// library's localtime and gmtime fill one buffer shared across the process.
std::tm local_time(std::time_t t);
std::tm utc_time(std::time_t t);

// This process's id, and whether the process `pid` is still running. A server an app started is
// told the app's (--parent-pid) and stops when it is gone, so an app that crashes leaves no
// server holding the port and a model's memory.
long current_pid();
bool process_alive(long pid);

// Open `url` in the default browser. Best effort.
void open_url(const std::string & url);

// True when a file manager started this process in a console of its own (a double click on
// Windows): nobody is there to read a URL, so the server opens the browser itself.
bool started_by_double_click();

// The signed-in user's home folder, or "" when there is none (a phone app's sandbox has no such
// thing a person edits): where the settings files of other programs live (connect.h).
std::string home_dir();

// This machine's IPv4 addresses on its local networks (neither loopback nor link-local), each
// once: what another device types to reach a server listening on every address.
std::vector<std::string> lan_addresses();

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

// The full path of `program` as the system would find it on PATH (with PATHEXT's extensions on
// Windows), or "" when it is not there. A name with a separator is a path and comes back as is when
// it exists.
std::string find_program(const std::string & program);

// A program the server talks to one line at a time on its stdin and stdout: a coding agent speaking
// the Agent Client Protocol, or a tool server speaking the Model Context Protocol (docs/agent.md).
// It and everything it starts run as one process group (a job on Windows), ended whole by stop() or
// the destructor, so a program never outlives what started it. Its stderr is discarded. Not on
// Android or iOS (can_run_commands()).
class LineProcess {
public:
    // `argv[0]` is found with find_program(); a Windows batch file (`npx.cmd`) runs through cmd.exe.
    // The program inherits this process's environment with `env` set on top of it. nullptr with
    // `err` set when it cannot start.
    static std::unique_ptr<LineProcess> start(const std::vector<std::string> & argv,
                                              const std::string & cwd,
                                              std::string & err,
                                              const std::map<std::string, std::string> & env = {});
    ~LineProcess();
    LineProcess(const LineProcess &) = delete;
    LineProcess & operator=(const LineProcess &) = delete;

    // One line, '\n' appended. False once the program's stdin is closed. Safe from any thread.
    bool write_line(const std::string & line);
    // The next line of its stdout, without the line break. Blocks; false at the end of its output
    // (it ended, or stop() was called). One reader at a time.
    bool read_line(std::string & line);
    // Ends the program and what it started; read_line() then returns false. Safe from any thread.
    void stop();

private:
    struct Impl;
    explicit LineProcess(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace bmoe::server::platform
