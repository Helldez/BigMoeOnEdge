// The Agent Client Protocol's wire (docs/agent.md, "Other agents"): JSON-RPC 2.0, one message a
// line, over a transport that is a program's stdin and stdout in the server and a pair of queues in
// a test. A request sent waits for its answer; a request the agent sends (a permission, a file read
// or write) runs its handler on a thread of its own, so one waiting for a person never holds up the
// messages behind it; a notification (session/update) runs its handler on the reading thread, in
// the order the agent sent it.
#pragma once

#include "json_util.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bmoe::server {

// One line at a time each way. read_line() blocks and returns false at the end; stop() ends the
// other side and makes a blocked read_line() return.
class AcpTransport {
public:
    virtual ~AcpTransport() = default;
    virtual bool write_line(const std::string & line) = 0;
    virtual bool read_line(std::string & line) = 0;
    virtual void stop() = 0;
};

class AcpConnection {
public:
    struct Handlers {
        // A request from the agent: true with `result`, or false with `error` (answered as a
        // JSON-RPC error). A method it does not know is answered "method not found" without it.
        std::function<bool(const std::string & method, const json & params, json & result, std::string & error)>
            request;
        std::function<void(const std::string & method, const json & params)> notification;
    };

    // Starts reading at once.
    AcpConnection(std::unique_ptr<AcpTransport> transport, Handlers handlers);
    // Stops the transport and joins every thread. A request handler still waiting (an approval)
    // must have been released by its owner first.
    ~AcpConnection();
    AcpConnection(const AcpConnection &) = delete;
    AcpConnection & operator=(const AcpConnection &) = delete;

    // Sends a request and waits for its answer. `cancelled` is asked every poll; the first time it
    // says true, `on_cancel` runs once (it sends session/cancel) and the wait goes on, for the agent
    // answers a cancelled prompt too (`cancelled` may close() the connection when it takes too long).
    // False with `err` when the agent answers an error or ends.
    bool call(const std::string & method,
              const json & params,
              json & result,
              std::string & err,
              const std::function<bool()> & cancelled = nullptr,
              const std::function<void()> & on_cancel = nullptr);
    void notify(const std::string & method, const json & params);
    // False once the agent's output has ended.
    bool alive() const { return alive_; }
    // Ends the agent now (its transport), failing whatever waits on it.
    void close();

private:
    struct Pending {
        bool done = false;
        bool ok = false;
        json result;
        std::string error;
    };

    void read_loop();
    void answer(const json & id, const json * result, const std::string & error, int code);
    bool send(const json & message);

    std::unique_ptr<AcpTransport> transport_;
    Handlers handlers_;
    std::atomic<bool> alive_{true};
    std::mutex m_; // pending_, next_id_, workers_
    std::condition_variable cv_;
    std::map<int64_t, Pending> pending_;
    int64_t next_id_ = 1;
    std::vector<std::thread> workers_;
    std::thread reader_;
};

} // namespace bmoe::server
