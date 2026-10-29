// JSON-RPC 2.0, one message a line, the wire both the Agent Client Protocol (another coding agent,
// docs/agent.md "Other agents") and the Model Context Protocol (a tool server, docs/agent.md "MCP
// servers") speak, over a transport that is a program's stdin and stdout in the server and a pair
// of queues in a test. A request sent waits for its answer; a request the other side sends (a
// permission, a file read or write) runs its handler on a thread of its own, so one waiting for a
// person never holds up the messages behind it; a notification runs its handler on the reading
// thread, in the order it was sent.
#pragma once

#include "json_util.h"
#include "platform.h"

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
class JsonRpcTransport {
public:
    virtual ~JsonRpcTransport() = default;
    virtual bool write_line(const std::string & line) = 0;
    virtual bool read_line(std::string & line) = 0;
    virtual void stop() = 0;
};

// A program started with platform::LineProcess as a transport: its stdin and stdout.
class ProcessTransport : public JsonRpcTransport {
public:
    explicit ProcessTransport(std::unique_ptr<platform::LineProcess> p) : p_(std::move(p)) {}
    bool write_line(const std::string & line) override { return p_->write_line(line); }
    bool read_line(std::string & line) override { return p_->read_line(line); }
    void stop() override { p_->stop(); }

private:
    std::unique_ptr<platform::LineProcess> p_;
};

class JsonRpcConnection {
public:
    struct Handlers {
        // A request from the other side: true with `result`, or false with `error` (answered as a
        // JSON-RPC error). A method it does not know is answered "method not found" without it.
        std::function<bool(const std::string & method, const json & params, json & result, std::string & error)>
            request;
        std::function<void(const std::string & method, const json & params)> notification;
    };

    // Starts reading at once.
    JsonRpcConnection(std::unique_ptr<JsonRpcTransport> transport, Handlers handlers);
    // Stops the transport and joins every thread. A request handler still waiting (an approval)
    // must have been released by its owner first.
    ~JsonRpcConnection();
    JsonRpcConnection(const JsonRpcConnection &) = delete;
    JsonRpcConnection & operator=(const JsonRpcConnection &) = delete;

    // Sends a request and waits for its answer. `cancelled` is asked every poll; the first time it
    // says true, `on_cancel` runs once (ACP sends session/cancel) and the wait goes on, for an agent
    // answers a cancelled prompt too (`cancelled` may close() the connection when it takes too long,
    // or at once). False with `err` when the other side answers an error or ends.
    bool call(const std::string & method,
              const json & params,
              json & result,
              std::string & err,
              const std::function<bool()> & cancelled = nullptr,
              const std::function<void()> & on_cancel = nullptr);
    void notify(const std::string & method, const json & params);
    // False once the other side's output has ended.
    bool alive() const { return alive_; }
    // Ends the other side now (its transport), failing whatever waits on it.
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

    std::unique_ptr<JsonRpcTransport> transport_;
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
