// The server's own command-line flags that more than one file reads: the parser in http_server.cpp
// and the service installer (service_spec.h), which completes an install's arguments with what a
// service needs. One name each, so the two cannot drift apart.
#pragma once

namespace bmoe::server::flag {

inline constexpr const char * host = "--host";
inline constexpr const char * api_key = "--api-key";
inline constexpr const char * api_key_file = "--api-key-file";
inline constexpr const char * no_api_key = "--no-api-key";
inline constexpr const char * data_dir = "--data-dir";
inline constexpr const char * log_file = "--log-file";
inline constexpr const char * autoload = "--autoload";
inline constexpr const char * open = "--open";
inline constexpr const char * parent_pid = "--parent-pid";

} // namespace bmoe::server::flag
