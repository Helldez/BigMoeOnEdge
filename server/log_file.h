// The server's log file (--log-file): what a service writes where a console would have been.
// Rotation is a pure rule over the file system, tested on its own; sending the process's output
// there is the platform's (platform::redirect_output).
#pragma once

#include <cstdint>
#include <string>

namespace bmoe::server {

// The size past which a start moves the log aside before writing: enough for weeks of a server's
// lines, small enough to read. One generation is kept (`<path>.1`), the older one replaced.
inline constexpr std::uintmax_t kLogRotateBytes = 16ull << 20;

// Moves `path` to `<path>.1` when it is at least `max_bytes`, replacing an earlier `.1`. A missing
// file, or one still under the size, is left as it is. False with `err` when the move fails.
bool rotate_log(const std::string & path, std::uintmax_t max_bytes, std::string & err);

} // namespace bmoe::server
