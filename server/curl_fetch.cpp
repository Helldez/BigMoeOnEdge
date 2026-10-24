#include "http_fetch.h"

#include "download_policy.h"
#include "url_entry.h"

// See model_library.cpp for why the subprocess header's chdir support is off.
#define SUBPROCESS_HAVE_CWD 0
#include "sheredom/subprocess.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace bmoe::server {

namespace {

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
