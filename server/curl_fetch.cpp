#include "http_fetch.h"

#include "download_policy.h"
#include "url_entry.h"

// See model_library.cpp for why the subprocess header's chdir support is off.
#define SUBPROCESS_HAVE_CWD 0
#include "sheredom/subprocess.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace bmoe::server {

namespace {

// How a streamed transfer that has nothing new is looked at again: the stop a caller raises through
// on_data is seen within this, as is curl's end.
constexpr auto kStreamPoll = std::chrono::milliseconds(20);
// Said to the sites the agent's web tools read: some refuse a request that names no browser-like
// agent, and a site owner can still tell what fetched the page.
constexpr const char * kAgentUserAgent = "Mozilla/5.0 (compatible; bmoe-agent)";

class CurlFetch final : public HttpFetch {
public:
    CurlFetch(std::string scratch, std::string endpoint)
        : scratch_(std::move(scratch)), endpoint_(std::move(endpoint)) {}

    bool get(const std::string & url,
             uint64_t from,
             uint64_t to,
             const std::string & token,
             std::string & body,
             std::string & error) override {
        // Only https, on every redirect too, or plain http under an http mirror the operator named.
        const std::string protos = curl_protocols(url, endpoint_);
        if (protos.empty()) {
            error = "not an https URL, nor one under the --hf-endpoint mirror";
            return false;
        }
        std::error_code ec;
        fs::create_directories(fs::u8path(scratch_), ec);
        const unsigned n = seq_.fetch_add(1);
        const fs::path out = fs::u8path(scratch_) / ("fetch-" + std::to_string(n) + ".bin");
        const fs::path hdr = fs::u8path(scratch_) / ("fetch-" + std::to_string(n) + ".hdr");
        // The token travels in a file curl reads, not on the command line: a process list shows
        // arguments to every local user. The file is this process's and is removed on return.
        if (!token.empty()) {
            std::ofstream h(hdr, std::ios::binary | std::ios::trunc);
            h << "Authorization: Bearer " << token << "\n";
            if (!h) {
                error = "could not write the request header file";
                return false;
            }
        }
        const std::string range = std::to_string(from) + "-" + std::to_string(to);
        const std::string out_s = out.string();
        const std::string hdr_arg = "@" + hdr.string();
        std::vector<const char *> cmd = {"curl",          "-sS",          "-L",      "--fail",
                                         "--max-time",    "180",          "--proto", protos.c_str(),
                                         "--proto-redir", protos.c_str(), "-o",      out_s.c_str()};
        if (to > 0) {
            cmd.push_back("-r");
            cmd.push_back(range.c_str());
        }
        if (!token.empty()) {
            cmd.push_back("-H");
            cmd.push_back(hdr_arg.c_str());
        }
        cmd.push_back("--"); // the URL is never read as an option
        cmd.push_back(url.c_str());
        cmd.push_back(nullptr);
        subprocess_s proc;
        const int opts = subprocess_option_search_user_path | subprocess_option_inherit_environment |
                         subprocess_option_no_window | subprocess_option_combined_stdout_stderr;
        bool ok = false;
        if (subprocess_create(cmd.data(), opts, &proc) != 0) {
            error = "could not start curl: is it installed and on PATH?";
        } else {
            int rc = 0;
            subprocess_join(&proc, &rc);
            std::string output;
            if (FILE * o = subprocess_stdout(&proc)) {
                char buf[256];
                while (std::fgets(buf, sizeof(buf), o))
                    output += buf;
            }
            subprocess_destroy(&proc);
            while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
                output.pop_back();
            if (rc != 0) {
                error = "curl exited with " + std::to_string(rc) + (output.empty() ? "" : ": " + output);
            } else {
                std::ifstream in(out, std::ios::binary);
                std::ostringstream ss;
                ss << in.rdbuf();
                body = ss.str();
                ok = true;
            }
        }
        fs::remove(out, ec);
        fs::remove(hdr, ec);
        return ok;
    }

    bool size(const std::string & url, uint64_t & bytes, std::string & error) override {
        bytes = 0;
        // The same protocol rule as every other fetch (download_policy.h): a pasted link is https by
        // then (url_entry.h), and the redirects stay on it.
        const std::string protos = curl_protocols(url, endpoint_);
        if (protos.empty()) {
            error = "not an https URL";
            return false;
        }
        std::error_code ec;
        fs::create_directories(fs::u8path(scratch_), ec);
        const fs::path out = fs::u8path(scratch_) / ("head-" + std::to_string(seq_.fetch_add(1)) + ".txt");
        const std::string out_s = out.string();
        // No --fail: every response's headers are written, the error's too, and head_answer reads
        // the status from them (curl's exit code over HTTP/2 does not say it).
        const char * cmd[] = {"curl",         "-sS",           "-I",           "-L", "--max-time",  "60", "--proto",
                              protos.c_str(), "--proto-redir", protos.c_str(), "-o", out_s.c_str(), "--", url.c_str(),
                              nullptr};
        subprocess_s proc;
        const int opts = subprocess_option_search_user_path | subprocess_option_inherit_environment |
                         subprocess_option_no_window | subprocess_option_combined_stdout_stderr;
        // No curl to ask with (a phone, whose app downloads for itself) is a size not known, as a host
        // that answers no HEAD is, never a refused link: the entry is made and its download, by the
        // server or by the client, says whether the file is there. A curl that starts and cannot
        // exec ends with no headers, which reads the same way below.
        if (subprocess_create(cmd, opts, &proc) != 0) return true;
        int rc = 0;
        subprocess_join(&proc, &rc);
        subprocess_destroy(&proc);
        std::ifstream in(out, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        in.close();
        fs::remove(out, ec);
        const HeadAnswer a = head_answer(ss.str());
        if (a.status >= 400) {
            error = "the link answered HTTP " + std::to_string(a.status);
            return false;
        }
        // No answer at all (a host that refuses HEAD, a network that drops it) leaves the size
        // unknown rather than the link refused: the download then says whether the file is there.
        if (rc == 0 && a.status >= 200 && a.status < 300) bytes = a.length;
        return true;
    }

    bool stream(const Request & req,
                const std::function<bool(const char *, size_t)> & on_data,
                int & status,
                std::string & error) override {
        status = 0;
        const bool https = req.url.compare(0, 8, "https://") == 0;
        const bool http = req.url.compare(0, 7, "http://") == 0;
        if (!https && !(http && req.allow_http)) {
            error = req.allow_http ? "not an http(s) URL" : "not an https URL";
            return false;
        }
        const char * protos = req.allow_http ? "=http,https" : "=https";
        std::error_code ec;
        fs::create_directories(fs::u8path(scratch_), ec);
        const std::string n = std::to_string(seq_.fetch_add(1));
        const fs::path hdr = fs::u8path(scratch_) / ("stream-" + n + ".req");
        const fs::path body = fs::u8path(scratch_) / ("stream-" + n + ".body");
        const fs::path got = fs::u8path(scratch_) / ("stream-" + n + ".hdr");
        struct Cleanup {
            std::vector<fs::path> files;
            ~Cleanup() {
                std::error_code e;
                for (const fs::path & f : files)
                    fs::remove(f, e);
            }
        } cleanup{{hdr, body, got}};
        // Headers, the key among them, and the body go in files curl reads: nothing a request
        // carries is on the command line, where every local user could read it.
        {
            std::ofstream h(hdr, std::ios::binary | std::ios::trunc);
            if (!req.token.empty()) h << "Authorization: Bearer " << req.token << "\n";
            if (!req.content_type.empty()) h << "Content-Type: " << req.content_type << "\n";
            if (!req.accept.empty()) h << "Accept: " << req.accept << "\n";
            if (!h) {
                error = "could not write the request header file";
                return false;
            }
        }
        const bool has_body = req.method == "POST";
        if (has_body) {
            std::ofstream b(body, std::ios::binary | std::ios::trunc);
            b << req.body;
            if (!b) {
                error = "could not write the request body file";
                return false;
            }
        }
        const std::string timeout = std::to_string(req.timeout_s);
        const std::string hdr_arg = "@" + hdr.string();
        const std::string body_arg = "@" + body.string();
        const std::string got_s = got.string();
        std::vector<const char *> cmd = {"curl",
                                         "-sS",
                                         "-L",
                                         "-N",
                                         "--max-time",
                                         timeout.c_str(),
                                         "--proto",
                                         protos,
                                         "--proto-redir",
                                         protos,
                                         "-A",
                                         kAgentUserAgent,
                                         "-H",
                                         hdr_arg.c_str(),
                                         "-D",
                                         got_s.c_str()};
        if (has_body) {
            cmd.push_back("--data-binary");
            cmd.push_back(body_arg.c_str());
        }
        cmd.push_back("--");
        cmd.push_back(req.url.c_str());
        cmd.push_back(nullptr);
        subprocess_s proc;
        const int opts = subprocess_option_search_user_path | subprocess_option_inherit_environment |
                         subprocess_option_no_window | subprocess_option_enable_async |
                         subprocess_option_enable_async_no_wait;
        if (subprocess_create(cmd.data(), opts, &proc) != 0) {
            error = "could not start curl: is it installed and on PATH?";
            return false;
        }
        char buf[8192];
        bool wanted = true;
        for (;;) {
            const unsigned got_n = subprocess_read_stdout(&proc, buf, sizeof(buf));
            if (got_n > 0) {
                if (!on_data(buf, got_n)) {
                    wanted = false;
                    break;
                }
                continue;
            }
            if (!subprocess_alive(&proc)) {
                // Ended: what it wrote before it ended is still in the pipe.
                unsigned m;
                while ((m = subprocess_read_stdout(&proc, buf, sizeof(buf))) > 0)
                    if (!on_data(buf, m)) break;
                break;
            }
            std::this_thread::sleep_for(kStreamPoll);
        }
        if (!wanted) subprocess_terminate(&proc);
        int rc = 0;
        subprocess_join(&proc, &rc);
        std::string errtext;
        if (FILE * e = subprocess_stderr(&proc)) {
            char line[256];
            while (std::fgets(line, sizeof(line), e))
                errtext += line;
        }
        subprocess_destroy(&proc);
        std::ifstream in(got, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        status = head_answer(ss.str()).status;
        if (status == 0 && wanted) {
            while (!errtext.empty() && (errtext.back() == '\n' || errtext.back() == '\r'))
                errtext.pop_back();
            error = errtext.empty() ? "curl exited with " + std::to_string(rc) : errtext;
            return false;
        }
        return true;
    }

    bool download(const std::string & url,
                  const std::string & dest,
                  const std::function<bool(uint64_t)> & progress,
                  std::string & error) override {
        // The feed's files are https, and so is every hop to them: no mirror rule applies here.
        const std::string protos = curl_protocols(url, "");
        if (protos.empty()) {
            error = "not an https URL";
            return false;
        }
        const fs::path out = fs::u8path(dest);
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        fs::remove(out, ec);
        const std::string out_s = out.string();
        // No --max-time: a pack is hundreds of megabytes. A transfer that stalls for a minute is
        // given up instead (--speed-limit/--speed-time), and curl retries what it can on its own.
        const char * cmd[] = {"curl",
                              "-sS",
                              "-L",
                              "--fail",
                              "--retry",
                              "3",
                              "--proto",
                              protos.c_str(),
                              "--proto-redir",
                              protos.c_str(),
                              "--speed-limit",
                              "1",
                              "--speed-time",
                              "60",
                              "-o",
                              out_s.c_str(),
                              "--",
                              url.c_str(),
                              nullptr};
        subprocess_s proc;
        const int opts = subprocess_option_search_user_path | subprocess_option_inherit_environment |
                         subprocess_option_no_window | subprocess_option_combined_stdout_stderr;
        if (subprocess_create(cmd, opts, &proc) != 0) {
            error = "could not start curl: is it installed and on PATH?";
            return false;
        }
        bool stopped = false;
        while (subprocess_alive(&proc)) {
            std::error_code sec;
            const uintmax_t got = fs::file_size(out, sec);
            if (progress && !progress(sec ? 0 : (uint64_t) got)) {
                subprocess_terminate(&proc);
                stopped = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        int rc = 0;
        subprocess_join(&proc, &rc);
        std::string output;
        if (FILE * o = subprocess_stdout(&proc)) {
            char buf[256];
            while (std::fgets(buf, sizeof(buf), o))
                output += buf;
        }
        subprocess_destroy(&proc);
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
            output.pop_back();
        if (stopped) {
            fs::remove(out, ec);
            error = "stopped";
            return false;
        }
        if (rc != 0) {
            fs::remove(out, ec);
            error = "curl exited with " + std::to_string(rc) + (output.empty() ? "" : ": " + output);
            return false;
        }
        return true;
    }

private:
    std::string scratch_;
    std::string endpoint_;
    std::atomic<unsigned> seq_{0};
};

} // namespace

std::unique_ptr<HttpFetch> curl_fetch(const std::string & scratch_dir, const std::string & endpoint) {
    return std::make_unique<CurlFetch>(scratch_dir, endpoint);
}

} // namespace bmoe::server
