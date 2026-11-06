// The server as a service of the person's init system: what it is, and the text each init system
// reads to run it. Pure: no file, no process, no platform. service_manager.cpp asks the machine
// which init system answers, writes what this renders where that one reads, and registers it.
//
// One service on every platform: a per-user agent, started at the person's login, restarted when
// it fails, with the server's arguments as the install gave them. A systemd user unit, a launchd
// agent and a Windows scheduled task are the three ways the platforms spell that.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace bmoe::server {

// The init system's name for the service, in launchd's reverse-DNS convention, which the other two
// accept as a plain name.
inline constexpr const char * kServiceLabel = "io.bigmoeonedge.server";
inline constexpr const char * kServiceDescription = "BigMoeOnEdge server";
// How long the init system waits before starting the server again after it fails, and how many
// times a scheduled task retries (systemd and launchd keep retrying).
inline constexpr int kServiceRestartDelayS = 5;
inline constexpr int kServiceRestartCount = 3;
// A scheduled task's priority: 4 is the first of the normal class (0 is highest, 7, the default,
// is below normal). A server that decodes a model is not a background chore.
inline constexpr int kTaskPriority = 4;

// The service's facts, the same whichever init system runs it.
struct ServiceSpec {
    std::string label = kServiceLabel;
    std::string executable;        // bmoe-server, absolute
    std::vector<std::string> args; // the server's arguments, one argv entry each
    std::string working_dir;       // the executable's folder: backends, the UI and the catalog sit beside it
    std::string log_file;          // where the init system sends the server's output before the server
                                   // redirects it itself (--log-file); "" where it keeps a journal
};

// The text each init system reads for `s`.
std::string render_systemd_unit(const ServiceSpec & s);
std::string render_launchd_plist(const ServiceSpec & s);
std::string render_task_scheduler_xml(const ServiceSpec & s);

// How each spells one argument on its command line: systemd's quoting (ExecStart), and the rules
// Windows programs parse their command line with (CommandLineToArgvW). launchd takes an array.
std::string systemd_quote(const std::string & arg);
// A path setting of a unit (WorkingDirectory=) is read as typed, quotes included: only the
// specifier character is escaped there.
std::string systemd_path(const std::string & path);
std::string windows_quote(const std::string & arg);
std::string xml_escape(const std::string & s);

// What the service starts the server with.
struct ServiceArguments {
    std::vector<std::string> args;
    // Non-empty: the key file the arguments name and the install must write first, with a key of
    // its own making, because the server is to listen beyond loopback and the install gave no key.
    std::string key_file;
};

// `given` with the value of every path-valued argument made absolute, the rest as typed. The
// service starts from the executable's folder, not the shell the install was typed in, so a
// relative model or folder would name a different place at the next login. `takes_path` says which
// flags take a path (the server's own and the engine's path parameters), `absolute` resolves one.
std::vector<std::string> with_absolute_paths(const std::vector<std::string> & given,
                                             const std::function<bool(const std::string &)> & takes_path,
                                             const std::function<std::string(const std::string &)> & absolute);

// `given` as typed at install, then what a service needs that the install did not say: the data
// folder, made explicit so the service and the person's shell agree on it; the log file; --autoload,
// since a service with nothing loaded serves nobody; and a key file when `--host` reaches beyond
// loopback with no key and no --no-api-key, since the launch rules would otherwise refuse the start.
// `--open` is dropped: a service has no browser to open. `data_dir` and `log_file` are absolute.
ServiceArguments
service_arguments(const std::vector<std::string> & given, const std::string & data_dir, const std::string & log_file);

// The name of the key file an install writes in the data folder.
inline constexpr const char * kServiceKeyFileName = "api-key";

} // namespace bmoe::server
