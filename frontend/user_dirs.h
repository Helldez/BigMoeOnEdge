// What every front-end (bmoe-cli, bmoe-server, the desktop app) asks the operating system the same way:
// where a person's data goes. One answer, so the folder one of them keeps a model's measurements in is
// the folder the others read them from (bmoe/model_records.h). No engine in it.
#pragma once

#include <string>

namespace bmoe::frontend {

// Per-user data directory for settings, downloaded models and each model's measurements (created on
// first use): %LOCALAPPDATA%\BigMoeOnEdge, ~/Library/Application Support/BigMoeOnEdge, or
// $XDG_DATA_HOME/bigmoeonedge (~/.local/share/bigmoeonedge).
std::string default_data_dir();

// Where each model's measurements are kept inside a data directory (bmoe/model_records.h).
std::string records_dir(const std::string & data_dir);

} // namespace bmoe::frontend
