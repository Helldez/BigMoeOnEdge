// `bmoe-server service <verb>`: the server registered with the person's init system as a service
// that starts at login and comes back after a failure (docs/server-api.md, "Running as a service").
//
// The service itself is service_spec.h, pure and tested as text. This is the adapter around it:
// which init system answers on this machine (asked of the machine: which of their command-line
// tools is here, never which platform this is), where that one reads a per-user service from, and
// the commands that register, start, stop and describe it.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace bmoe::server {

// What the verbs need of the server's own argument parser: parse `args` as a start would and say
// why the server could not start with them, or "". An argument that ends the server before it
// serves (--help, --version) is a refusal too: a service runs none of them.
using ArgsValidator = std::function<std::string(const std::vector<std::string> & args)>;

// argv[0] is the verb (install, uninstall, start, stop, status), the rest its arguments: for
// install, the server's own options and engine parameters, as a start would take them. Returns the
// process's exit code.
int run_service_command(int argc, char ** argv, const ArgsValidator & validate);

void print_service_usage();

} // namespace bmoe::server
