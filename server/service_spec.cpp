#include "service_spec.h"

#include "access.h"
#include "server_flags.h"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace bmoe::server {

std::string systemd_quote(const std::string & arg) {
    // systemd's own quoting: double quotes, with backslash, the quote itself and the two characters
    // it expands ($ for variables, % for specifiers) escaped so the argument arrives as typed.
    std::string out = "\"";
    for (const char c : arg) {
        if (c == '\\' || c == '"') out += '\\';
        if (c == '$') out += '$';
        if (c == '%') out += '%';
        out += c;
    }
    out += '"';
    return out;
}

std::string systemd_path(const std::string & path) {
    std::string out;
    for (const char c : path) {
        if (c == '%') out += '%';
        out += c;
    }
    return out;
}

std::string windows_quote(const std::string & arg) {
    // What CommandLineToArgvW undoes: an argument with no space or quote passes as is; otherwise it
    // is wrapped in quotes, a quote inside is \", and backslashes right before a quote (or the closing
    // one) are doubled, since only those are read as escapes.
    const bool plain = !arg.empty() && arg.find_first_of(" \t\"") == std::string::npos;
    if (plain) return arg;
    std::string out = "\"";
    size_t backslashes = 0;
    for (const char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
        } else {
            out.append(backslashes, '\\');
            out += c;
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

std::string xml_escape(const std::string & s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::string render_systemd_unit(const ServiceSpec & s) {
    std::string exec = systemd_quote(s.executable);
    for (const std::string & a : s.args)
        exec += " " + systemd_quote(a);
    std::string unit;
    unit += "[Unit]\n";
    unit += "Description=" + std::string(kServiceDescription) + "\n";
    unit += "\n[Service]\n";
    unit += "ExecStart=" + exec + "\n";
    if (!s.working_dir.empty()) unit += "WorkingDirectory=" + systemd_path(s.working_dir) + "\n";
    // A failure is started again; a stop asked for is not. The journal keeps the output the server
    // writes before --log-file takes it, and whatever a crash leaves.
    unit += "Restart=on-failure\n";
    unit += "RestartSec=" + std::to_string(kServiceRestartDelayS) + "\n";
    unit += "\n[Install]\n";
    unit += "WantedBy=default.target\n";
    return unit;
}

std::string render_launchd_plist(const ServiceSpec & s) {
    std::string p;
    p += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    p += "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
         "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    p += "<plist version=\"1.0\">\n<dict>\n";
    p += "  <key>Label</key>\n  <string>" + xml_escape(s.label) + "</string>\n";
    p += "  <key>ProgramArguments</key>\n  <array>\n";
    p += "    <string>" + xml_escape(s.executable) + "</string>\n";
    for (const std::string & a : s.args)
        p += "    <string>" + xml_escape(a) + "</string>\n";
    p += "  </array>\n";
    if (!s.working_dir.empty())
        p += "  <key>WorkingDirectory</key>\n  <string>" + xml_escape(s.working_dir) + "</string>\n";
    // Started at login and again after a failure, not after a stop asked for. ThrottleInterval is
    // the restart delay. ProcessType Standard: launchd throttles a Background process's I/O and
    // CPU, which is the opposite of what a server reading experts from flash needs.
    p += "  <key>RunAtLoad</key>\n  <true/>\n";
    p += "  <key>KeepAlive</key>\n  <dict>\n    <key>SuccessfulExit</key>\n    <false/>\n  </dict>\n";
    p += "  <key>ThrottleInterval</key>\n  <integer>" + std::to_string(kServiceRestartDelayS) + "</integer>\n";
    p += "  <key>ProcessType</key>\n  <string>Standard</string>\n";
    if (!s.log_file.empty()) {
        p += "  <key>StandardOutPath</key>\n  <string>" + xml_escape(s.log_file) + "</string>\n";
        p += "  <key>StandardErrorPath</key>\n  <string>" + xml_escape(s.log_file) + "</string>\n";
    }
    p += "</dict>\n</plist>\n";
    return p;
}

std::string render_task_scheduler_xml(const ServiceSpec & s) {
    std::string args;
    for (const std::string & a : s.args) {
        if (!args.empty()) args += ' ';
        args += windows_quote(a);
    }
    std::string x;
    x += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    x += "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n";
    x += "  <RegistrationInfo>\n    <Description>" + xml_escape(kServiceDescription) +
         "</Description>\n  </RegistrationInfo>\n";
    x += "  <Triggers>\n    <LogonTrigger>\n      <Enabled>true</Enabled>\n    </LogonTrigger>\n  </Triggers>\n";
    // S4U: the task runs as the person who registered it, with no password stored and no window of
    // its own, whether or not they are signed in at the desktop. The one cost is that the server
    // cannot reach network shares with their credentials, which it has no reason to.
    x += "  <Principals>\n    <Principal id=\"Author\">\n      <LogonType>S4U</LogonType>\n"
         "      <RunLevel>LeastPrivilege</RunLevel>\n    </Principal>\n  </Principals>\n";
    x += "  <Settings>\n";
    x += "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n";
    x += "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n";
    x += "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n";
    x += "    <AllowHardTerminate>true</AllowHardTerminate>\n";
    x += "    <StartWhenAvailable>true</StartWhenAvailable>\n";
    x += "    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\n";
    x += "    <IdleSettings>\n      <StopOnIdleEnd>false</StopOnIdleEnd>\n"
         "      <RestartOnIdle>false</RestartOnIdle>\n    </IdleSettings>\n";
    x += "    <AllowStartOnDemand>true</AllowStartOnDemand>\n";
    x += "    <Enabled>true</Enabled>\n";
    x += "    <Hidden>false</Hidden>\n";
    x += "    <RunOnlyIfIdle>false</RunOnlyIfIdle>\n";
    x += "    <WakeToRun>false</WakeToRun>\n";
    // No time limit: the default stops a task after three days.
    x += "    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n";
    x += "    <Priority>" + std::to_string(kTaskPriority) + "</Priority>\n";
    x += "    <RestartOnFailure>\n      <Interval>PT1M</Interval>\n      <Count>" +
         std::to_string(kServiceRestartCount) + "</Count>\n    </RestartOnFailure>\n";
    x += "  </Settings>\n";
    x += "  <Actions Context=\"Author\">\n    <Exec>\n";
    x += "      <Command>" + xml_escape(s.executable) + "</Command>\n";
    if (!args.empty()) x += "      <Arguments>" + xml_escape(args) + "</Arguments>\n";
    if (!s.working_dir.empty()) x += "      <WorkingDirectory>" + xml_escape(s.working_dir) + "</WorkingDirectory>\n";
    x += "    </Exec>\n  </Actions>\n";
    x += "</Task>\n";
    return x;
}

std::vector<std::string> with_absolute_paths(const std::vector<std::string> & given,
                                             const std::function<bool(const std::string &)> & takes_path,
                                             const std::function<std::string(const std::string &)> & absolute) {
    std::vector<std::string> out;
    out.reserve(given.size());
    for (size_t i = 0; i < given.size(); ++i) {
        out.push_back(given[i]);
        if (takes_path(given[i]) && i + 1 < given.size()) out.push_back(absolute(given[++i]));
    }
    return out;
}

ServiceArguments
service_arguments(const std::vector<std::string> & given, const std::string & data_dir, const std::string & log_file) {
    const auto has = [&given](const char * f) { return std::find(given.begin(), given.end(), f) != given.end(); };
    ServiceArguments out;
    std::string host;
    for (size_t i = 0; i < given.size(); ++i) {
        if (given[i] == flag::open) continue;
        if (given[i] == flag::host && i + 1 < given.size()) host = given[i + 1];
        out.args.push_back(given[i]);
    }
    if (!has(flag::data_dir)) {
        out.args.push_back(flag::data_dir);
        out.args.push_back(data_dir);
    }
    if (!has(flag::log_file)) {
        out.args.push_back(flag::log_file);
        out.args.push_back(log_file);
    }
    if (!has(flag::autoload)) out.args.push_back(flag::autoload);
    const bool keyed = has(flag::api_key) || has(flag::api_key_file) || has(flag::no_api_key);
    if (!host.empty() && !is_loopback_host(host) && !keyed) {
        out.key_file = (fs::u8path(data_dir) / kServiceKeyFileName).u8string();
        out.args.push_back(flag::api_key_file);
        out.args.push_back(out.key_file);
    }
    return out;
}

} // namespace bmoe::server
