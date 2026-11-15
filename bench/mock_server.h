#ifndef BENCH_MOCK_SERVER_H
#define BENCH_MOCK_SERVER_H

// A loopback OpenAI-compatible endpoint for the binary driver. It serves the
// scenario's scripted assistant replies in order and records the paths it saw.
// No network beyond 127.0.0.1; no external process.

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent/tool.h"

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
    void serve();
    void handle(int fd);
    bool next_reply(MockReply& out);
    void send_all(int fd, const std::string& body, const char* content_type);

    std::deque<MockReply> script_;
    mutable std::mutex script_mtx_;
    mutable std::mutex paths_mtx_;
    std::vector<std::string> paths_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

} // namespace bench

#endif // BENCH_MOCK_SERVER_H
