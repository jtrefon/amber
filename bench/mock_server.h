#ifndef BENCH_MOCK_SERVER_H
#define BENCH_MOCK_SERVER_H

// A loopback OpenAI-compatible endpoint for the binary driver. It serves the
// scenario's scripted assistant replies in order and records the paths it saw.
// No network beyond 127.0.0.1; no external process.
//
// This is a thin FIFO compatibility layer over ScriptedEndpoint. New code uses
// ScriptedEndpoint directly; MockServer stays so existing callers keep their
// scripted-reply API.

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "agent/tool.h"
#include "bench/scripted_endpoint.h"

namespace bench {

struct MockReply {
    std::string content;
    agent::json tool_calls = agent::json::array();
    long prompt_tokens = 0;
    long completion_tokens = 0;
};

class MockServer {
public:
    explicit MockServer(std::deque<MockReply> script);
    ~MockServer();

    MockServer(const MockServer&) = delete;
    MockServer& operator=(const MockServer&) = delete;

    bool start();
    void stop();
    std::string base_url() const;
    std::vector<std::string> paths() const;

private:
    std::unique_ptr<ScriptedEndpoint> endpoint_;
};

} // namespace bench

#endif // BENCH_MOCK_SERVER_H
