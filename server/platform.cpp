#include "platform.h"

#if !defined(_WIN32)
// See model_library.cpp for why the subprocess header's chdir support is off. Windows opens a URL
// through the shell's own call instead, so the header is left out there.
#define SUBPROCESS_HAVE_CWD 0
#include "sheredom/subprocess.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <climits>
#else
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <climits>
#endif

#if !defined(_WIN32)
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace bmoe::server::platform {

namespace {

#if defined(_WIN32)
std::string narrow(const std::wstring & w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string & s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), w.data(), n);
    return w;
}
#endif

} // namespace

json host_info() {
    unsigned long long total = 0, avail = 0;
#if defined(_WIN32)
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        total = ms.ullTotalPhys;
        avail = ms.ullAvailPhys;
    }
#elif defined(__APPLE__)
    uint64_t mem = 0;
    size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) total = mem;
    vm_statistics64_data_t vs;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t) &vs, &count) == KERN_SUCCESS)
        avail = (unsigned long long) (vs.free_count + vs.inactive_count) * (unsigned long long) vm_page_size;
#else
    std::ifstream in("/proc/meminfo");
    std::string key;
    unsigned long long kb = 0;
    std::string unit;
    while (in >> key >> kb >> unit) {
        if (key == "MemTotal:") total = kb * 1024ull;
        if (key == "MemAvailable:") avail = kb * 1024ull;
    }
#endif
    return json{{"ram_total_mib", total >> 20},
                {"ram_available_mib", avail >> 20},
                {"cpu_threads", std::thread::hardware_concurrency()}};
}

std::string executable_dir() {
#if defined(_WIN32)
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD) buf.size());
        if (n == 0) return "";
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return fs::path(buf).parent_path().u8string();
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return "";
    std::error_code ec;
    return fs::canonical(buf, ec).parent_path().u8string();
#else
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? "" : p.parent_path().u8string();
#endif
}

std::string default_data_dir() {
#if defined(_WIN32)
    PWSTR w = nullptr;
    std::string base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &w))) base = narrow(w);
    CoTaskMemFree(w);
    return (fs::u8path(base) / "BigMoeOnEdge").u8string();
#elif defined(__APPLE__)
    const char * home = std::getenv("HOME");
    return (fs::path(home ? home : ".") / "Library" / "Application Support" / "BigMoeOnEdge").u8string();
#else
    const char * xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) return (fs::path(xdg) / "bigmoeonedge").u8string();
    const char * home = std::getenv("HOME");
    return (fs::path(home ? home : ".") / ".local" / "share" / "bigmoeonedge").u8string();
#endif
}

namespace {
// A value someone set stays; the engine's defaults only fill what is empty.
void set_env_default(const char * name, const std::string & value) {
#if defined(_WIN32)
    if (!std::getenv(name)) _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 0);
#endif
}
} // namespace

void prepare_compute_backends(const std::string & data_dir) {
    // Where a backend may keep compiled programs between runs (the OpenCL kernels, for one). Its
    // own default is a temporary directory, which an app's process may not be able to write, and
    // the cache then disables itself. Read when the backend is loaded, so set before that.
    std::error_code ec;
    const fs::path cache = fs::u8path(data_dir) / "backend-cache";
    fs::create_directories(cache, ec);
    if (!ec) set_env_default("GGML_OPENCL_KERNEL_CACHE_DIR", cache.u8string());
#if defined(__ANDROID__)
    // Polling halves the cost of each crossing to the DSP, and the prefill arena stops the graph at
    // every layer, twice (measured on device). The backend polls only while a DSP op is in flight,
    // so neither costs anything until the planner arms the NPU, and neither is read where the
    // Hexagon backend is not built in. setenv with overwrite 0 leaves a value someone set alone.
    set_env_default("GGML_HEXAGON_OPPOLL", "1");
    set_env_default("GGML_HEXAGON_HOSTBUF", "1");
#endif
}

const char * build_target() {
#if defined(BMOE_BUILD_TARGET)
    return BMOE_BUILD_TARGET;
#else
    return "";
#endif
}

bool library_loads(const std::string & name) {
    if (name.empty()) return false;
#if defined(_WIN32)
    // The system's own folders only: a driver's library is installed there, and a file of that
    // name beside the program or in the working directory is not the driver.
    HMODULE h = LoadLibraryExW(widen(name).c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) return false;
    FreeLibrary(h);
    return true;
#else
    void * h = dlopen(name.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!h) return false;
    dlclose(h);
    return true;
#endif
}

void prepare_backend_modules(const std::vector<std::string> & module_paths) {
#if defined(_WIN32)
    // Loaded with its own folder searched for what it links (the vendor runtime the pack carries),
    // and kept loaded: ggml's own load of the same path then finds it in the process.
    for (const std::string & p : module_paths)
        LoadLibraryExW(fs::u8path(p).wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    (void) module_paths;
#endif
}

std::tm local_time(std::time_t t) {
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

std::tm utc_time(std::time_t t) {
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm;
}

long current_pid() {
#if defined(_WIN32)
    return (long) GetCurrentProcessId();
#else
    return (long) getpid();
#endif
}

bool process_alive(long pid) {
    if (pid <= 0) return false;
#if defined(_WIN32)
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD) pid);
    if (!h) return false;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
#else
    // A process of another user answers EPERM: it is there.
    return kill((pid_t) pid, 0) == 0 || errno == EPERM;
#endif
}

void open_url(const std::string & url) {
#if defined(_WIN32)
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    // An app there has no browser to start and no shell to start it with; the app is the UI.
    (void) url;
#else
    // The opener is started directly, the URL one argument of it: no shell reads it. Its output is
    // read and dropped on a thread of its own, which also reaps it, so neither the opener nor a
    // browser it starts can block on a full pipe and the server never waits for either.
#if defined(__APPLE__)
    const char * opener = "open";
#else
    const char * opener = "xdg-open";
#endif
    const char * cmd[] = {opener, url.c_str(), nullptr};
    auto * proc = new subprocess_s;
    const int opts = subprocess_option_search_user_path | subprocess_option_inherit_environment |
                     subprocess_option_combined_stdout_stderr;
    if (subprocess_create(cmd, opts, proc) != 0) {
        delete proc;
        return;
    }
    std::thread([proc] {
        if (FILE * out = subprocess_stdout(proc)) {
            char buf[256];
            while (std::fread(buf, 1, sizeof(buf), out) > 0) {
            }
        }
        int rc = 0;
        subprocess_join(proc, &rc);
        subprocess_destroy(proc);
        delete proc;
    }).detach();
#endif
}

std::string home_dir() {
#if defined(_WIN32)
    PWSTR w = nullptr;
    std::string home;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &w))) home = narrow(w);
    CoTaskMemFree(w);
    return home;
#elif defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    return "";
#else
    const char * home = std::getenv("HOME");
    return home ? home : "";
#endif
}

const char * os_name() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__ANDROID__)
    return "Android";
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    return "iOS";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}

bool can_run_commands() {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    return false;
#else
    return true;
#endif
}

namespace {
// How often a running command is looked at for its deadline and a stop while it writes nothing.
constexpr int kCommandPollMs = 50;

void keep_output(CommandResult & r, const char * data, size_t n, size_t max_output) {
    const size_t room = r.output.size() < max_output ? max_output - r.output.size() : 0;
    if (n > room) r.truncated = true;
    r.output.append(data, n < room ? n : room);
}
} // namespace

CommandResult run_command(const std::string & command,
                          const std::string & cwd,
                          int timeout_s,
                          size_t max_output,
                          const std::atomic<bool> & stop) {
    CommandResult r;
    if (!can_run_commands()) {
        r.error = "this platform does not let the server start a command";
        return r;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    const auto overdue = [&] {
        if (stop.load())
            r.stopped = true;
        else if (std::chrono::steady_clock::now() >= deadline)
            r.timed_out = true;
        return r.stopped || r.timed_out;
    };
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        r.error = "could not create a pipe for the command";
        return r;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    // /d skips AutoRun, /s keeps the quoting of the command as written.
    std::wstring line = L"cmd.exe /d /s /c \"" + widen(command) + L"\"";
    const std::wstring wcwd = widen(cwd);
    // A job holds the command and everything it starts, so a timeout or a stop ends them all.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
    const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                                   nullptr, wcwd.c_str(), &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        if (job) CloseHandle(job);
        r.error = "could not start cmd.exe";
        return r;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    r.started = true;
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break; // every writer closed it
        if (avail > 0) {
            DWORD n = 0;
            if (!ReadFile(rd, buf, (DWORD) std::min<size_t>(sizeof(buf), avail), &n, nullptr) || n == 0) break;
            keep_output(r, buf, n, max_output);
            continue;
        }
        if (overdue()) break;
        if (WaitForSingleObject(pi.hProcess, kCommandPollMs) == WAIT_OBJECT_0) {
            // Ended: read what is left, without waiting on a child it left holding the pipe.
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                DWORD n = 0;
                if (!ReadFile(rd, buf, (DWORD) std::min<size_t>(sizeof(buf), avail), &n, nullptr) || n == 0) break;
                keep_output(r, buf, n, max_output);
            }
            break;
        }
    }
    if (r.stopped || r.timed_out) {
        if (job)
            TerminateJobObject(job, 1);
        else
            TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
    } else {
        DWORD code = 0;
        WaitForSingleObject(pi.hProcess, INFINITE);
        if (GetExitCodeProcess(pi.hProcess, &code)) r.exit_code = (int) code;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    if (job) CloseHandle(job); // KILL_ON_JOB_CLOSE: whatever the command left running ends here
    return r;
#else
#if defined(__ANDROID__)
    const char * shell = "/system/bin/sh";
#else
    const char * shell = "/bin/sh";
#endif
    int fds[2];
    if (pipe(fds) != 0) {
        r.error = "could not create a pipe for the command";
        return r;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    // Everything the child needs is made before fork: between fork and exec it may only make calls
    // that are safe in a copy of a threaded process.
    const char * c_cwd = cwd.c_str();
    const char * c_cmd = command.c_str();
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        r.error = "could not start a process";
        return r;
    }
    if (pid == 0) {
        setpgid(0, 0);
        if (chdir(c_cwd) != 0) _exit(126);
        const int in = open("/dev/null", O_RDONLY);
        if (in >= 0) dup2(in, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        execl(shell, "sh", "-c", c_cmd, (char *) nullptr);
        _exit(127);
    }
    setpgid(pid, pid); // also here: whichever of the two runs first, the group exists before a kill
    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    r.started = true;
    char buf[4096];
    bool eof = false;
    int status = 0;
    bool reaped = false;
    while (!eof || !reaped) {
        if (overdue()) break;
        if (!eof) {
            pollfd p{fds[0], POLLIN, 0};
            const int pr = poll(&p, 1, kCommandPollMs);
            if (pr > 0) {
                const ssize_t n = read(fds[0], buf, sizeof(buf));
                if (n > 0)
                    keep_output(r, buf, (size_t) n, max_output);
                else if (n == 0 || (errno != EAGAIN && errno != EINTR))
                    eof = true;
            }
        }
        if (!reaped) {
            const pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid)
                reaped = true;
            else if (eof)
                std::this_thread::sleep_for(std::chrono::milliseconds(kCommandPollMs));
        }
        // The shell is gone and the pipe is still open: a background child holds it. The command
        // has answered; what it left running is ended below with its group.
        if (reaped && !eof) {
            ssize_t n;
            while ((n = read(fds[0], buf, sizeof(buf))) > 0)
                keep_output(r, buf, (size_t) n, max_output);
            break;
        }
    }
    close(fds[0]);
    // The whole group, whatever happened: a command never outlives its tool call.
    kill(-pid, SIGKILL);
    if (!reaped) waitpid(pid, &status, 0);
    if (!r.stopped && !r.timed_out && WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
    return r;
#endif
}

bool started_by_double_click() {
#if defined(_WIN32)
    // A terminal the user already had open also holds the console; one Explorer created for us
    // holds only us.
    DWORD pid;
    return GetConsoleProcessList(&pid, 1) == 1;
#else
    return false;
#endif
}

} // namespace bmoe::server::platform
