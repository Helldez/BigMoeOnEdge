// What the server needs from the network to add a model from a repository: a small listing, and
// the first bytes of a file. A port, so the policy that reads a repository (hf_repo.h,
// model_inspect.h) is tested with a fake and the one real adapter is the system's curl, as the
// downloads already use it (model_library.h): no TLS stack in the server, certificates the OS's.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bmoe::server {

class HttpFetch {
public:
    virtual ~HttpFetch() = default;
    // GET `url` into `body`. With `to` > 0 only the bytes [from, to] are asked for (a range the
    // server honours or not: the caller checks the size it got). `token` goes as a bearer token
    // when set, and is never written anywhere a process list or a log would show it. False with
    // the reason, which names no token either.
    virtual bool get(const std::string & url,
                     uint64_t from,
                     uint64_t to,
                     const std::string & token,
                     std::string & body,
                     std::string & error) = 0;
    // The size of the file at `url`, from a HEAD request that follows redirects. True with
    // `bytes` 0 when the size is unknown: an adapter that cannot ask (this default) still lets a
    // model be added, and its download then runs without a total. False, with the reason, when the
    // server answered that there is no such file.
    virtual bool size(const std::string & url, uint64_t & bytes, std::string & error) {
        (void) url;
        (void) error;
        bytes = 0;
        return true;
    }
    // GET `url` into the file at `dest` (UTF-8), whole, replacing it. `progress` is told the bytes
    // received so far, every half second or so, and stops the transfer by returning false. https
    // only, on every redirect too. False, with the reason, when the file did not arrive whole. An
    // adapter that cannot write files (this default) says so: the server then offers no download
    // of a pack or an update.
    virtual bool download(const std::string & url,
                          const std::string & dest,
                          const std::function<bool(uint64_t received)> & progress,
                          std::string & error) {
        (void) url;
        (void) dest;
        (void) progress;
        error = "this build's network adapter does not download files";
        return false;
    }
};

// The system's curl. `scratch_dir` is a writable folder for the bytes in flight (created on
// first use). `endpoint` is the repository endpoint: https is fetched anywhere, plain http only
// under it (download_policy.h: curl_protocols). Whether curl starts at all is only known when it
// is asked.
std::unique_ptr<HttpFetch> curl_fetch(const std::string & scratch_dir, const std::string & endpoint);

} // namespace bmoe::server
