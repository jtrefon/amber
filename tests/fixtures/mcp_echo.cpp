// MCP stdio test server (C++ replacement for mcp_echo.py): a real, tiny MCP
// server over newline-framed JSON-RPC on stdin/stdout.
//
// Usage:
//   mcp_echo              — serve tools/resources/prompts
//   mcp_echo stderr       — write one line to stderr before serving
//   mcp_echo boom         — write a fatal line to stderr and exit 1
//
// Behavior matches the Python fixture it replaces, so the transport tests are
// byte-for-byte equivalent: no Python runtime is needed to build or run the
// test suite.
#include <nlohmann/json.hpp>

#include <cstdio>
#include <iostream>
#include <string>

using json = nlohmann::json;

namespace {

json text_property() {
    return json::object({{"type", "string"}, {"description", "text to echo"}});
}

json echo_tool() {
    return json::object({{"name", "echo_tool"},
                         {"description", "Echo the arguments back"},
                         {"inputSchema",
                          json::object(
                              {{"type", "object"},
                               {"properties",
                                json::object({{"text", text_property()}})},
                               {"required", json::array({"text"})}})}});
}

json greet_resource() {
    return json::object({{"uri", "doc://greet"},
                         {"name", "Greeting"},
                         {"description", "A greeting document"},
                         {"mimeType", "text/plain"}});
}

json greet_argument() {
    return json::object({{"name", "name"},
                         {"description", "who to greet"},
                         {"required", true}});
}

json greet_prompt() {
    return json::object({{"name", "greet"},
                         {"title", "Greet"},
                         {"description", "Generate a greeting"},
                         {"arguments", json::array({greet_argument()})}});
}

const json kInitResult = json::object(
    {{"protocolVersion", "2025-06-18"},
     {"capabilities",
      json::object({{"tools", json::object({{"listChanged", true}})},
                    {"resources", json::object()},
                    {"prompts", json::object()}})},
     {"serverInfo", json::object({{"name", "echo"}, {"version", "1.0"}})}});

const json kTools = json::array({echo_tool()});
const json kResources = json::array({greet_resource()});
const json kPrompts = json::array({greet_prompt()});

// Strip a trailing carriage return left by CRLF framing on the wire.
void strip_cr(std::string& s) {
    if (!s.empty() && s.back() == '\r') s.pop_back();
}

json handle_request(const std::string& method, const json& params) {
    if (method == "tools/call") {
        json args = params.value("arguments", json::object());
        std::string text = args.value("text", std::string());
        return json::object({{"content",
                              json::array({json::object({{"type", "text"},
                                                         {"text", "echo:" + text}})})}});
    }
    if (method == "resources/read") {
        std::string uri = params.value("uri", std::string());
        return json::object({{"contents",
                              json::array({json::object({{"uri", uri},
                                                         {"mimeType", "text/plain"},
                                                         {"text", "hello " + uri}})})}});
    }
    if (method == "prompts/get") {
        json args = params.value("arguments", json::object());
        std::string name = args.value("name", std::string("world"));
        return json::object({{"messages",
                              json::array({json::object(
                                  {{"role", "user"},
                                   {"content",
                                    json::object({{"type", "text"},
                                                  {"text", "greet " + name}})}})})}});
    }
    return json::object({{"echo",
                          json::object({{"method", method}, {"params", params}})}});
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "stderr") {
        std::fputs("hello stderr\n", stderr);
        std::fflush(stderr);
    }
    if (mode == "boom") {
        std::fputs("fatal startup error\n", stderr);
        std::fflush(stderr);
        return 1;
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        strip_cr(line);
        if (line.empty()) continue;
        json obj = json::parse(line, nullptr, false);
        if (obj.is_discarded()) continue;
        if (!obj.contains("id") || obj["id"].is_null()) continue;
        json reqid = obj["id"];
        std::string method = obj.value("method", std::string());
        json params = obj.value("params", json::object());

        json result = (method == "initialize")
                          ? kInitResult
                          : (method == "tools/list")
                                ? json{{"tools", kTools}}
                                : (method == "resources/list")
                                      ? json{{"resources", kResources}}
                                      : (method == "prompts/list")
                                            ? json{{"prompts", kPrompts}}
                                            : handle_request(method, params);
        std::cout << json{{"jsonrpc", "2.0"}, {"id", reqid},
                          {"result", result}}.dump()
                  << "\n";
        std::cout.flush();
    }
    return 0;
}