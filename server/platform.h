// What the server needs from the operating system. The only file in server/ with platform #ifs.
// Built as a library of its own (bmoe_server_platform) with nothing of the engine in it, so the
// desktop app (desktop/) asks the same functions where the data folder is and how to open a URL.
#pragma once

#include <nlohmann/json.hpp>

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

} // namespace bmoe::server::platform
