#include "json_rpc.h"

#include <chrono>

namespace bmoe::server {

namespace {
// How often a waiting call looks at its `cancelled`.
constexpr auto kCancelPoll = std::chrono::milliseconds(100);
// JSON-RPC's own error codes.
constexpr int kMethodNotFound = -32601;
constexpr int kInternalError = -32603;
} // namespace

JsonRpcConnection::JsonRpcConnection(std::unique_ptr<JsonRpcTransport> transport, Handlers handlers)
    : transport_(std::move(transport)), handlers_(std::move(handlers)) {
    reader_ = std::thread([this] { read_loop(); });
}

JsonRpcConnection::~JsonRpcConnection() {
    close();
    if (reader_.joinable()) reader_.join();
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lk(m_);
        workers.swap(workers_);
    }
    for (std::thread & t : workers)
        if (t.joinable()) t.join();
}

void JsonRpcConnection::close() {
    transport_->stop();
}

bool JsonRpcConnection::send(const json & message) {
    // One line: dump() never writes a raw line break, the wire's one rule.
    return transport_->write_line(message.dump(-1, ' ', false, json::error_handler_t::replace));
}

void JsonRpcConnection::notify(const std::string & method, const json & params) {
    send(json{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
}

bool JsonRpcConnection::call(const std::string & method,
                             const json & params,
                             json & result,
                             std::string & err,
                             const std::function<bool()> & cancelled,
                             const std::function<void()> & on_cancel) {
    int64_t id = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        id = next_id_++;
        pending_[id] = Pending{};
    }
    if (!alive_ || !send(json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}})) {
        std::lock_guard<std::mutex> lk(m_);
        pending_.erase(id);
        err = "the program is not running";
        return false;
    }
    bool told = false;
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        if (pending_[id].done) break;
        if (!alive_) {
            pending_.erase(id);
            err = "the program ended before it answered " + method;
            return false;
        }
        if (cancelled) {
            lk.unlock();
            const bool now = cancelled();
            if (now && !told && on_cancel) on_cancel();
            told = told || now;
            lk.lock();
            if (pending_[id].done) break;
        }
        cv_.wait_for(lk, kCancelPoll);
    }
    Pending p = std::move(pending_[id]);
    pending_.erase(id);
    if (!p.ok) {
        err = p.error;
        return false;
    }
    result = std::move(p.result);
    return true;
}

void JsonRpcConnection::answer(const json & id, const json * result, const std::string & error, int code) {
    json m{{"jsonrpc", "2.0"}, {"id", id}};
    if (result)
        m["result"] = *result;
    else
        m["error"] = json{{"code", code}, {"message", error}};
    send(m);
}

void JsonRpcConnection::read_loop() {
    std::string line;
    while (transport_->read_line(line)) {
        if (line.empty()) continue;
        json m = json::parse(line, nullptr, false);
        if (m.is_discarded() || !m.is_object()) continue; // not a message: a stray line, dropped
        const bool has_id = m.contains("id") && !m["id"].is_null();
        const std::string method = m.value("method", "");
        const json params = m.value("params", json::object());
        if (!method.empty() && has_id) {
            // A request: answered from a thread of its own, since its handler may wait for a person.
            const json id = m["id"];
            std::lock_guard<std::mutex> lk(m_);
            workers_.emplace_back([this, id, method, params] {
                json result;
                std::string error;
                if (!handlers_.request) {
                    answer(id, nullptr, "method not found: " + method, kMethodNotFound);
                    return;
                }
                if (handlers_.request(method, params, result, error))
                    answer(id, &result, "", 0);
                else
                    answer(id, nullptr, error,
                           error.rfind("method not found", 0) == 0 ? kMethodNotFound : kInternalError);
            });
        } else if (!method.empty()) {
            if (handlers_.notification) handlers_.notification(method, params);
        } else if (has_id && m["id"].is_number_integer()) {
            const int64_t id = m["id"].get<int64_t>();
            std::lock_guard<std::mutex> lk(m_);
            const auto it = pending_.find(id);
            if (it == pending_.end()) continue;
            it->second.done = true;
            if (m.contains("error")) {
                const json & e = m["error"];
                it->second.error = e.is_object() ? e.value("message", std::string("refused")) : e.dump();
            } else {
                it->second.ok = true;
                it->second.result = m.value("result", json::object());
            }
            cv_.notify_all();
        }
    }
    alive_ = false;
    std::lock_guard<std::mutex> lk(m_);
    cv_.notify_all();
}

} // namespace bmoe::server
