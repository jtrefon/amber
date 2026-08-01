
#ifndef AGENT_MCP_TEST_UTIL_H
#define AGENT_MCP_TEST_UTIL_H

// Shared test doubles for the MCP test files: a scripted in-memory transport
// and JSON helpers for scripting initialize/discovery responses.

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "agent/mcp_transport.h"

namespace mcp_test {

// Scripted in-memory transport: every request pops the next scripted result.
class FakeTransport : public agent::McpTransport {
public:
    std::vector<std::string> methods;
    std::vector<int> responded_errors;
    int initialized_notifications = 0;
    std::deque<agent::McpTransportResult> script;
    bool fail_requests = false;

    agent::McpTransportResult request(int id, const std::string& method,
                                      const json& params) override {
        (void)id;
        (void)params;
        methods.push_back(method);
        if (fail_requests) {
            agent::McpTransportResult r;
            r.status = agent::McpTransportStatus::TransportError;
            return r;
        }
        if (script.empty()) {
            agent::McpTransportResult r;
            r.status = agent::McpTransportStatus::TransportError;
            return r;
        }
        agent::McpTransportResult r = std::move(script.front());
        script.pop_front();
        return r;
    }

    bool notify(const std::string& method, const json& params) override {
        (void)params;
        if (method == "notifications/initialized") ++initialized_notifications;
        return true;
    }

    bool respond(int id, const json& result) override {
        (void)id;
        (void)result;
        return true;
    }

    bool respond_error(int id, const agent::McpError& error) override {
        (void)id;
        responded_errors.push_back(error.code);
        return true;
    }

    void shutdown() override {}
    std::string failure_reason() const override {
        return fail_requests ? "fake failure" : "";
    }

    void deliver_server_message(const agent::McpMessage& m) {
        if (on_server_message_) on_server_message_(m);
    }
};

inline agent::McpTransportResult ok_result(json result) {
    agent::McpTransportResult r;
    agent::McpMessage m;
    m.id = 1;
    m.result = std::move(result);
    r.message = std::move(m);
    return r;
}

inline agent::McpTransportResult err_result(const std::string& message) {
    agent::McpTransportResult r;
    agent::McpMessage m;
    m.id = 1;
    agent::McpError e;
    e.code = -32602;
    e.message = message;
    m.error = e;
    r.message = std::move(m);
    return r;
}

inline agent::McpTransportResult timeout_result() {
    agent::McpTransportResult r;
    r.status = agent::McpTransportStatus::Timeout;
    return r;
}

inline json init_result(const std::string& version = "2025-06-18") {
    return {{"protocolVersion", version},
            {"capabilities",
             {{"tools", {{"listChanged", true}}},
              {"resources", json::object()},
              {"prompts", json::object()}}},
            {"serverInfo", {{"name", "fixture"}, {"version", "1.0"}}}};
}

// Script the connect sequence: initialize + three discovery pages.
inline void script_connect(FakeTransport& t) {
    t.script.push_back(ok_result(init_result()));
    t.script.push_back(ok_result({{"tools", json::array()}}));
    t.script.push_back(ok_result({{"resources", json::array()}}));
    t.script.push_back(ok_result({{"prompts", json::array()}}));
}

} // namespace mcp_test

#endif // AGENT_MCP_TEST_UTIL_H
