#include "service_manager.h"

#include "platform.h"
#include "server_flags.h"
#include "service_spec.h"

#include "bmoe/params.h"

// See model_library.cpp for why the subprocess header's chdir support is off.
#define SUBPROCESS_HAVE_CWD 0
#include "sheredom/subprocess.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace bmoe::server {

namespace {

// What one command of the init system did: its exit code and everything it printed.
struct Exec {
    bool started = false;
    int code = -1;
    std::string output;
};

Exec run(const std::vector<std::string> & argv) {
    Exec e;
    std::vector<const char *> cmd;
    for (const std::string & a : argv)
        cmd.push_back(a.c_str());
    cmd.push_back(nullptr);
    subprocess_s proc;
    const int opts = subprocess_option_search_user_path | subprocess_option_inherit_environment |
                     subprocess_option_no_window | subprocess_option_combined_stdout_stderr;
    if (subprocess_create(cmd.data(), opts, &proc) != 0) return e;
    e.started = true;
    if (FILE * out = subprocess_stdout(&proc)) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, out)) > 0)
            e.output.append(buf, n);
    }
    subprocess_join(&proc, &e.code);
    subprocess_destroy(&proc);
    return e;
}

std::string joined(const std::vector<std::string> & argv) {
    std::string s;
    for (const std::string & a : argv)
        s += (s.empty() ? "" : " ") + a;
    return s;
}

// One init system the machine may run: where it reads a per-user service from, and its verbs.
class InitSystem {
public:
    virtual ~InitSystem() = default;
    virtual const char * name() const = 0;
    // Where the rendered service goes. `data_dir` is for an init system with no folder of its own
    // for the text (a scheduled task is registered from a file and kept beside the data).
    virtual std::string unit_path(const std::string & data_dir) const = 0;
    virtual std::string render(const ServiceSpec & s) const = 0;
    // Whether the init system keeps the server's own output itself (a journal), so the service
    // names no file for it. The server's --log-file is there either way.
    virtual bool keeps_journal() const { return false; }
    virtual Exec install(const std::string & unit) = 0; // register the written text, enabled, started
    virtual Exec uninstall(const std::string & unit) = 0;
    // `unit` again: an init system that keeps the service alive stops it by taking the written
    // text out of the session, and starts it by putting it back.
    virtual Exec start(const std::string & unit) = 0;
    virtual Exec stop(const std::string & unit) = 0;
    virtual Exec status() = 0;
    // How a person reads what the init system kept, when it keeps anything.
    virtual std::string journal_hint() const { return ""; }
};

class Systemd : public InitSystem {
public:
    const char * name() const override { return "systemd (user)"; }
    std::string unit_path(const std::string &) const override {
        const std::string cfg = platform::user_config_dir();
        if (cfg.empty()) return "";
        return (fs::u8path(cfg) / "systemd" / "user" / (std::string(kServiceLabel) + ".service")).u8string();
    }
    std::string render(const ServiceSpec & s) const override { return render_systemd_unit(s); }
    bool keeps_journal() const override { return true; }
    Exec install(const std::string &) override {
        Exec e = run({"systemctl", "--user", "daemon-reload"});
        if (!e.started || e.code != 0) return e;
        e = run({"systemctl", "--user", "enable", "--now", kServiceLabel});
        if (!e.started || e.code != 0) return e;
        // A user's services stop with their last session unless the user lingers: on a box nobody
        // signs in to, that is the difference between a server and nothing. Asked for, not
        // required: where it needs an administrator, the hint says what to run.
        const Exec linger = run({"loginctl", "enable-linger"});
        if (!linger.started || linger.code != 0)
            e.output += "note: could not enable lingering, so the service stops with your last session; an "
                        "administrator can run: loginctl enable-linger $USER\n";
        return e;
    }
    Exec uninstall(const std::string & unit) override {
        Exec e = run({"systemctl", "--user", "disable", "--now", kServiceLabel});
        std::error_code ec;
        fs::remove(fs::u8path(unit), ec);
        run({"systemctl", "--user", "daemon-reload"});
        return e;
    }
    Exec start(const std::string &) override { return run({"systemctl", "--user", "start", kServiceLabel}); }
    Exec stop(const std::string &) override { return run({"systemctl", "--user", "stop", kServiceLabel}); }
    Exec status() override { return run({"systemctl", "--user", "status", kServiceLabel, "--no-pager"}); }
    std::string journal_hint() const override { return "journalctl --user -u " + std::string(kServiceLabel); }
};

class Launchd : public InitSystem {
    // The person's own launchd domain, where a LaunchAgent is bootstrapped.
    static std::string domain() {
#if !defined(_WIN32)
        return "gui/" + std::to_string((long) getuid());
#else
        return "gui/0";
#endif
    }
    static std::string target() { return domain() + "/" + kServiceLabel; }

public:
    const char * name() const override { return "launchd"; }
    std::string unit_path(const std::string &) const override {
        const std::string cfg = platform::user_config_dir();
        if (cfg.empty()) return "";
        return (fs::u8path(cfg) / "LaunchAgents" / (std::string(kServiceLabel) + ".plist")).u8string();
    }
    std::string render(const ServiceSpec & s) const override { return render_launchd_plist(s); }
    Exec install(const std::string & unit) override {
        run({"launchctl", "bootout", target()}); // an earlier registration of the same label goes first
        return run({"launchctl", "bootstrap", domain(), unit});
    }
    Exec uninstall(const std::string & unit) override {
        // A stopped agent is already out of the session: bootout then has nothing to do, and the
        // uninstall is the plist going.
        Exec e = run({"launchctl", "bootout", target()});
        std::error_code ec;
        fs::remove(fs::u8path(unit), ec);
        if (e.started && !fs::exists(fs::u8path(unit), ec)) e.code = 0;
        return e;
    }
    // KeepAlive starts the server again after any end but a clean exit, a signal included, so a
    // stop is the agent taken out of the session (it comes back at the next login, the plist being
    // still there) and a start is the agent put back, or kicked when it is already in.
    Exec start(const std::string & unit) override {
        const Exec e = run({"launchctl", "bootstrap", domain(), unit});
        if (e.started && e.code == 0) return e;
        return run({"launchctl", "kickstart", target()});
    }
    Exec stop(const std::string &) override { return run({"launchctl", "bootout", target()}); }
    Exec status() override {
        // `launchctl print` says everything about the service; the lines a person asks for are
        // whether it runs, as what, and how it last ended.
        Exec e = run({"launchctl", "print", target()});
        if (!e.started || e.code != 0) return e;
        std::istringstream in(e.output);
        std::string line, kept;
        while (std::getline(in, line))
            for (const char * key : {"state =", "pid =", "last exit", "program =", "runs =", "path ="})
                if (line.find(key) != std::string::npos) {
                    kept += line + "\n";
                    break;
                }
        e.output = kept;
        return e;
    }
};

class TaskScheduler : public InitSystem {
public:
    const char * name() const override { return "Task Scheduler"; }
    std::string unit_path(const std::string & data_dir) const override {
        return (fs::u8path(data_dir) / "service" / (std::string(kServiceLabel) + ".xml")).u8string();
    }
    std::string render(const ServiceSpec & s) const override { return render_task_scheduler_xml(s); }
    Exec install(const std::string & unit) override {
        Exec e = run({"schtasks", "/Create", "/TN", kServiceLabel, "/XML", unit, "/F"});
        if (!e.started || e.code != 0) return e;
        return run({"schtasks", "/Run", "/TN", kServiceLabel});
    }
    Exec uninstall(const std::string & unit) override {
        run({"schtasks", "/End", "/TN", kServiceLabel});
        Exec e = run({"schtasks", "/Delete", "/TN", kServiceLabel, "/F"});
        std::error_code ec;
        fs::remove(fs::u8path(unit), ec);
        return e;
    }
    Exec start(const std::string &) override { return run({"schtasks", "/Run", "/TN", kServiceLabel}); }
    Exec stop(const std::string &) override { return run({"schtasks", "/End", "/TN", kServiceLabel}); }
    Exec status() override { return run({"schtasks", "/Query", "/TN", kServiceLabel, "/V", "/FO", "LIST"}); }
};

// The init system whose tool answers on this machine, asked in turn; null when none does.
std::unique_ptr<InitSystem> detect_init_system() {
    if (!platform::find_program("launchctl").empty()) return std::make_unique<Launchd>();
    if (!platform::find_program("systemctl").empty()) return std::make_unique<Systemd>();
    if (!platform::find_program("schtasks").empty()) return std::make_unique<TaskScheduler>();
    return nullptr;
}

// A key of this install's making: 32 random bytes as hex, from the system's own source.
std::string make_key() {
    std::random_device rd;
    static const char * digits = "0123456789abcdef";
    std::string key;
    for (int i = 0; i < 32; ++i) {
        const unsigned v = rd();
        key += digits[(v >> 4) & 15];
        key += digits[v & 15];
    }
    return key;
}

// Writes `text` to `path`, its folders made, readable by its owner only where the file system has
// owners. False with `err` when it cannot.
bool write_private(const std::string & path, const std::string & text, std::string & err) {
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    fs::create_directories(p.parent_path(), ec);
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "cannot write " + path;
            return false;
        }
        out << text;
    }
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    return true;
}

bool write_text(const std::string & path, const std::string & text, std::string & err) {
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "cannot write " + path;
        return false;
    }
    out << text;
    return true;
}

std::string absolute(const std::string & p) {
    std::error_code ec;
    const fs::path a = fs::absolute(fs::u8path(p), ec);
    return ec ? p : a.lexically_normal().u8string();
}

// The data folder the verb's arguments name, or the platform's default.
std::string data_dir_of(const std::vector<std::string> & args) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == flag::data_dir) return absolute(args[i + 1]);
    return absolute(platform::default_data_dir());
}

int report(const Exec & e, const char * what) {
    if (!e.started) {
        std::fprintf(stderr, "bmoe-server service: could not run the init system's tool to %s\n", what);
        return 1;
    }
    if (!e.output.empty()) std::printf("%s", e.output.c_str());
    if (e.code != 0) std::fprintf(stderr, "bmoe-server service: %s failed (exit %d)\n", what, e.code);
    return e.code;
}

// Whether `arg` is a flag whose value is a path: one of the server's own, or an engine parameter
// the table types as a path (by its long or short flag).
bool takes_path(const std::string & arg) {
    for (const char * f : flag::path_valued)
        if (arg == f) return true;
    for (const ParamDesc & d : params())
        if (d.type == ParamType::Path && (arg == d.flag || (!d.short_flag.empty() && arg == d.short_flag))) return true;
    return false;
}

int do_install(InitSystem & init, const std::vector<std::string> & typed, const ArgsValidator & validate) {
    const std::string exe = platform::executable_path();
    if (exe.empty()) {
        std::fprintf(stderr, "bmoe-server service: cannot tell this executable's own path\n");
        return 1;
    }
    // The service starts from the executable's folder: a path typed relative to this shell is made
    // absolute here, or the install passes and the start at login finds nothing where it looks.
    const std::vector<std::string> given = with_absolute_paths(typed, takes_path, absolute);
    const std::string data_dir = data_dir_of(given);
    const std::string log_file = (fs::u8path(data_dir) / "logs" / "bmoe-server.log").u8string();
    const ServiceArguments sa = service_arguments(given, data_dir, log_file);
    {
        // The init system opens the log file itself where it keeps no journal, and makes no folder.
        std::error_code ec;
        fs::create_directories(fs::u8path(log_file).parent_path(), ec);
    }

    std::string err;
    std::string key;
    if (!sa.key_file.empty()) {
        // A key file an earlier install wrote is kept: the clients that hold its key keep working.
        std::error_code ec;
        if (!fs::exists(fs::u8path(sa.key_file), ec)) {
            key = make_key();
            if (!write_private(sa.key_file, key + "\n", err)) {
                std::fprintf(stderr, "bmoe-server service: %s\n", err.c_str());
                return 1;
            }
        }
    }
    // The arguments the service would start with, parsed exactly as that start will parse them: a
    // mistake is refused here, with its reason, not at the next login with nobody reading.
    const std::string refusal = validate(sa.args);
    if (!refusal.empty()) {
        std::fprintf(stderr, "bmoe-server service: the service could not start with these arguments: %s\n",
                     refusal.c_str());
        return 2;
    }

    ServiceSpec spec;
    spec.executable = exe;
    spec.args = sa.args;
    spec.working_dir = fs::u8path(exe).parent_path().u8string();
    spec.log_file = init.keeps_journal() ? "" : log_file;
    const std::string unit = init.unit_path(data_dir);
    if (unit.empty()) {
        std::fprintf(stderr, "bmoe-server service: cannot tell where %s reads a per-user service from\n", init.name());
        return 1;
    }
    if (!write_text(unit, init.render(spec), err)) {
        std::fprintf(stderr, "bmoe-server service: %s\n", err.c_str());
        return 1;
    }
    std::printf("bmoe-server service: registered with %s\n", init.name());
    std::printf("  service: %s\n", unit.c_str());
    std::printf("  runs:    %s %s\n", exe.c_str(), joined(sa.args).c_str());
    std::printf("  log:     %s\n", log_file.c_str());
    if (!sa.key_file.empty())
        std::printf("  API key: %s (clients send it as Authorization: Bearer ...)\n",
                    key.empty() ? ("kept in " + sa.key_file).c_str() : key.c_str());
    std::fflush(stdout);
    const int rc = report(init.install(unit), "register and start the service");
    if (rc != 0) {
        // A registration the init system refused is not left behind: `status` would call it
        // registered, and the next login would try it again with nobody reading.
        std::error_code ec;
        fs::remove(fs::u8path(unit), ec);
    }
    return rc;
}

} // namespace

void print_service_usage() {
    std::printf("usage: bmoe-server service <verb> [server options]\n"
                "\n"
                "  install [options]   register this server with the init system as a service of yours,\n"
                "                      started at login and again after a failure, with these server\n"
                "                      options and engine parameters (as a start would take them).\n"
                "                      Adds what a service needs: --autoload, --log-file, the data\n"
                "                      folder, and a key file when --host reaches beyond loopback\n"
                "  uninstall           stop it and remove the registration (settings and models stay)\n"
                "  start | stop        now, without changing the registration\n"
                "  status              what the init system says of it, and where its files are\n"
                "\n"
                "A verb other than install takes --data-dir PATH when the install was given one.\n"
                "Looks for systemd (a user unit), launchd (an agent) or Task Scheduler (a task).\n");
}

int run_service_command(int argc, char ** argv, const ArgsValidator & validate) {
    if (argc < 1 || std::string(argv[0]) == "-h" || std::string(argv[0]) == "--help") {
        print_service_usage();
        return argc < 1 ? 2 : 0;
    }
    const std::string verb = argv[0];
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);

    std::unique_ptr<InitSystem> init = detect_init_system();
    if (!init) {
        std::fprintf(stderr, "bmoe-server service: no init system this server can register with answers here "
                             "(looked for launchctl, systemctl and schtasks)\n");
        return 1;
    }
    const std::string data_dir = data_dir_of(args);
    const std::string unit = init->unit_path(data_dir);

    if (verb == "install") return do_install(*init, args, validate);
    if (verb == "uninstall") {
        const int rc = report(init->uninstall(unit), "unregister the service");
        if (rc == 0) std::printf("bmoe-server service: removed from %s (%s)\n", init->name(), unit.c_str());
        return rc;
    }
    if (verb == "start") return report(init->start(unit), "start the service");
    if (verb == "stop") return report(init->stop(unit), "stop the service");
    if (verb == "status") {
        std::error_code ec;
        const bool registered = fs::exists(fs::u8path(unit), ec);
        std::printf("init system: %s\n", init->name());
        std::printf("service:     %s%s\n", unit.c_str(), registered ? "" : " (not installed)");
        std::printf("log:         %s\n", (fs::u8path(data_dir) / "logs" / "bmoe-server.log").u8string().c_str());
        if (!init->journal_hint().empty()) std::printf("journal:     %s\n", init->journal_hint().c_str());
        std::fflush(stdout);
        if (!registered) return 3;
        // An init system answers a registered service that is not running with a non-zero exit
        // (systemd 3, launchd "could not find service"): that is a state, not a failure.
        const Exec e = init->status();
        if (!e.started) return report(e, "read the service's status");
        if (!e.output.empty()) std::printf("%s", e.output.c_str());
        if (e.code != 0) {
            std::printf(
                "state:       not running (stopped, or not started at this login; `service start` starts it)\n");
            return 3;
        }
        return 0;
    }
    std::fprintf(stderr, "bmoe-server service: unknown verb '%s'\n", verb.c_str());
    print_service_usage();
    return 2;
}

} // namespace bmoe::server
