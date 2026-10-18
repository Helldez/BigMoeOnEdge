// What the server needs from the network to add a model from a repository: a small listing, and
// the first bytes of a file. A port, so the policy that reads a repository (hf_repo.h,
// model_inspect.h) is tested with a fake and the one real adapter is the system's curl, as the
// downloads already use it (model_library.h): no TLS stack in the server, certificates the OS's.
#pragma once

#include <cstdint>
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
};

// The system's curl. `scratch_dir` is a writable folder for the bytes in flight (created on
// first use). Whether curl starts at all is only known when it is asked.
std::unique_ptr<HttpFetch> curl_fetch(const std::string & scratch_dir);

} // namespace bmoe::server
