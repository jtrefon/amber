
#ifndef AGENT_MCP_TRANSPORT_STDIO_H
#define AGENT_MCP_TRANSPORT_STDIO_H

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent/mcp_transport.h"
#include "agent/process.h"

namespace agent {

// MCP stdio transport: spawns the server as a subprocess (direct exec, three
// separate pipes, own process group) and speaks newline-delimited JSON-RPC
// over its stdin/stdout. Server stderr is drained separately and retained
// (capped) for failure diagnostics. Shutdown follows the MCP ladder: close
// stdin, SIGTERM, then SIGKILL (see docs/spec/mcp/mcp-transport.md).
class StdioTransport : public McpTransport {
public:
    // Spawns `command` immediately. On spawn failure the transport is dead;
    // failure_reason() reports the error and every call fails fast.
    StdioTransport(std::string command, std::vector<std::string> args,
                   std::string cwd,
                   std::function<void(const McpMessage&)> on_server_message =
                       {},
                   int request_timeout_ms = 60000,
                   const CancellationToken* cancel_token = nullptr);
    ~StdioTransport() override;

    McpTransportResult request(int id, const std::string& method,
                               const json& params) override;
    bool notify(const std::string& method, const json& params) override;
    bool respond(int id, const json& result) override;
    bool respond_error(int id, const McpError& error) override;
    void shutdown() override;
    std::string failure_reason() const override;

private:
    void stdout_loop();
    void stderr_loop();
    void handle_line(const std::string& line);
    bool write_line(const std::string& line);
    void terminate_child() const;
    void fail(const std::string& reason);

    std::string command_;
    std::vector<std::string> args_;
    std::string cwd_;
    int request_timeout_ms_;
    const CancellationToken* cancel_token_ = nullptr;
    pid_t pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    int stderr_fd_ = -1;

    std::atomic<bool> closed_{false};
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::map<int, McpMessage> pending_;
    std::string failure_;
    std::string stderr_tail_;
    std::thread stdout_thread_;
    std::thread stderr_thread_;
};

} // namespace agent

#endif // AGENT_MCP_TRANSPORT_STDIO_H
