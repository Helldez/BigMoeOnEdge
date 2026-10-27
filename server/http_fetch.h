// What the server needs from the network to add a model from a repository: a small listing, and
// the first bytes of a file. A port, so the policy that reads a repository (hf_repo.h,
// model_inspect.h) is tested with a fake and the one real adapter is the system's curl, as the
// downloads already use it (model_library.h): no TLS stack in the server, certificates the OS's.
#pragma once

#include <cstddef>
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

    // One request whose answer is read as it arrives: the agent's web tools and a provider's
    // streamed chat (agent.h). `on_data` sees each piece of the body and returns false to end the
    // transfer there (enough has been read, or the run was stopped). `status` is the HTTP status of
    // the last response (after redirects); a 4xx or 5xx still delivers its body, which says why.
    // `token` goes as a bearer token, by the same rule as get(): never on a command line or in a log.
    struct Request {
        std::string method = "GET";
        std::string url;
        std::string token;
        std::string body;         // sent as is, for a POST
        std::string content_type; // of `body`
        std::string accept;       // the Accept header, when set
        int timeout_s = 60;       // the whole transfer
        // Plain http is fetched too (a provider on this machine or the local network); otherwise
        // https only, on every redirect.
        bool allow_http = false;
    };
    // False with the reason when the request could not be made at all (no network adapter, curl
    // missing, a scheme refused); true once a response was read, whatever its status. This default
    // is an adapter that cannot make one.
    virtual bool stream(const Request & req,
                        const std::function<bool(const char * data, size_t n)> & on_data,
                        int & status,
                        std::string & error) {
        (void) req;
        (void) on_data;
        status = 0;
        error = "this server cannot make web requests";
        return false;
    }
};

// The system's curl. `scratch_dir` is a writable folder for the bytes in flight (created on
// first use). `endpoint` is the repository endpoint: https is fetched anywhere, plain http only
// under it (download_policy.h: curl_protocols). Whether curl starts at all is only known when it
// is asked.
std::unique_ptr<HttpFetch> curl_fetch(const std::string & scratch_dir, const std::string & endpoint);

} // namespace bmoe::server
