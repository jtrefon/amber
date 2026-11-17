#ifndef BENCH_SCRIPTED_ENDPOINT_H
#define BENCH_SCRIPTED_ENDPOINT_H

// A loopback HTTP endpoint shared by amber-bench, the CLI E2E driver and the
// TUI pty harness. It serves scripted replies in order or delegates to a
// responder that can inspect the request. No network beyond 127.0.0.1.

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent/llm.h"
#include "agent/tool.h"

namespace bench {

struct EndpointRequest {
    std::string method;
    std::string path;
    std::string body;
    agent::json json = agent::json::object();
    std::vector<agent::Message> messages;
    agent::json tools = agent::json::array();
    bool stream = false;
    int sequence = -1; // chat-request index; -1 for a non-chat path
    std::map<std::string, std::string> headers;
};

struct EndpointReply {
    bool handled = true; // false: use the endpoint's built-in path handling
    int status = 200;
    std::string content_type = "application/json";
    std::string body; // raw body; wins over content/tool_calls
    std::string content;
    agent::json tool_calls = agent::json::array();
    std::string reasoning;
    long prompt_tokens = 0;
    long completion_tokens = 0;
    std::string finish_reason;
    int delay_ms = 0;
    int drop_after_chunks = 0;
    bool malformed_json = false;
    bool disconnect = false;
    int retry_after_s = 0;
    std::vector<std::string> sse_chunks; // raw SSE frames; empty = build from content
};

using EndpointResponder = std::function<EndpointReply(const EndpointRequest&)>;

class ScriptedEndpoint {
public:
    explicit ScriptedEndpoint(std::deque<EndpointReply> script);
    explicit ScriptedEndpoint(EndpointResponder responder);
    ~ScriptedEndpoint();

    ScriptedEndpoint(const ScriptedEndpoint&) = delete;
    ScriptedEndpoint& operator=(const ScriptedEndpoint&) = delete;

    bool start();
    void stop();
    std::string root_url() const;
    std::string base_url() const;
    std::vector<std::string> paths() const;
    std::vector<EndpointRequest> requests() const;
    int chat_calls() const;
    void reset_chat();

private:
    void serve();
    void handle(int fd);
    EndpointReply respond(const EndpointRequest& request);
    EndpointReply next_reply();
    void send_reply(int fd, const EndpointRequest& request, const EndpointReply& reply);
    void send_all(int fd, const std::string& body, const std::string& content_type, int status,
                  int retry_after_s);

    std::deque<EndpointReply> script_;
    EndpointResponder responder_;
    mutable std::mutex script_mtx_;
    mutable std::mutex log_mtx_;
    std::vector<EndpointRequest> requests_;
    std::vector<std::string> paths_;
    int chat_calls_ = 0;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

// Parse an OpenAI/Anthropic/Gemini JSON body into the agent's message shape.
std::vector<agent::Message> endpoint_messages(const agent::json& body);

// Whether a request path is a chat completion, as opposed to a catalogue or an
// unknown path the endpoint answers itself.
bool endpoint_is_chat_path(const std::string& path);

// Request-shape views shared by the trace matcher and the binary request-log
// contract: the system text (OpenAI messages, Anthropic `system`, Gemini
// `systemInstruction`), the role sequence and the offered tool names.
std::string endpoint_system_text(const agent::json& body);
std::vector<std::string> endpoint_roles(const agent::json& body);
std::vector<std::string> endpoint_tool_names(const agent::json& body);

} // namespace bench

#endif // BENCH_SCRIPTED_ENDPOINT_H
