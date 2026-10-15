
#ifndef AGENT_MCP_TEST_UTIL_H
#define AGENT_MCP_TEST_UTIL_H

// Shared test doubles for the MCP test files: a scripted in-memory transport
// and JSON helpers for scripting initialize/discovery responses.

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "agent/extensions.h"
#include "agent/mcp_transport.h"
#include "agent/registry.h"
#include "agent/tool.h"

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
        if (method == "notifications/initialized")
            ++initialized_notifications;
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
    std::string failure_reason() const override { return fail_requests ? "fake failure" : ""; }

    void deliver_server_message(const agent::McpMessage& m) {
        if (on_server_message_)
            on_server_message_(m);
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
// The port the runtime hands a plugin, as a test double: owner-tagged
// registration into a local registry, with the names recorded. The real
// (thread-safe, runtime-owned) implementation is exercised end to end by
// `mcp_feature_arrives_and_leaves_as_one_plugin` in the runtime suite.
class TestToolPort : public agent::ToolPort {
public:
    TestToolPort(agent::ToolRegistry& registry, std::string owner)
        : registry_(registry), owner_(std::move(owner)) {}

    void add(std::unique_ptr<agent::Tool> tool, agent::ToolMeta meta = {}) override {
        if (!tool)
            return;
        const std::string name = tool->name();
        registry_.register_tool(std::move(tool), owner_, std::move(meta));
        names_.push_back(name);
    }

    void remove(const std::string& name) override {
        const auto it = std::find(names_.begin(), names_.end(), name);
        if (it == names_.end())
            return; // never carried by this port
        registry_.remove_owned_tool(name, owner_);
        names_.erase(it);
    }

    void open() override {}
    void close() override {
        for (const auto& name : names_)
            registry_.remove_owned_tool(name, owner_);
        names_.clear();
    }

private:
    agent::ToolRegistry& registry_;
    std::string owner_;
    std::vector<std::string> names_;
};

inline void script_connect(FakeTransport& t) {
    t.script.push_back(ok_result(init_result()));
    t.script.push_back(ok_result({{"tools", json::array()}}));
    t.script.push_back(ok_result({{"resources", json::array()}}));
    t.script.push_back(ok_result({{"prompts", json::array()}}));
}

} // namespace mcp_test

#endif // AGENT_MCP_TEST_UTIL_H
