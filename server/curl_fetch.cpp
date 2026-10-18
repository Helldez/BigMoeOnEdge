#include "http_fetch.h"

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
    explicit CurlFetch(std::string scratch) : scratch_(std::move(scratch)) {}

    bool get(const std::string & url,
             uint64_t from,
             uint64_t to,
             const std::string & token,
             std::string & body,
             std::string & error) override {
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
        std::vector<const char *> cmd = {"curl", "-sS", "-L", "--fail", "--max-time", "180", "-o", out_s.c_str()};
        if (to > 0) {
            cmd.push_back("-r");
            cmd.push_back(range.c_str());
        }
        if (!token.empty()) {
            cmd.push_back("-H");
            cmd.push_back(hdr_arg.c_str());
        }
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

private:
    std::string scratch_;
    std::atomic<unsigned> seq_{0};
};

} // namespace

std::unique_ptr<HttpFetch> curl_fetch(const std::string & scratch_dir) {
    return std::make_unique<CurlFetch>(scratch_dir);
}

} // namespace bmoe::server
