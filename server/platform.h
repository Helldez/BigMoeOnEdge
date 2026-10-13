// What the server needs from the operating system. The only file in server/ with platform #ifs.
#pragma once

#include "json_util.h"

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

// Open `url` in the default browser. Best effort.
void open_url(const std::string & url);

// True when a file manager started this process in a console of its own (a double click on
// Windows): nobody is there to read a URL, so the server opens the browser itself.
bool started_by_double_click();

} // namespace bmoe::server::platform
